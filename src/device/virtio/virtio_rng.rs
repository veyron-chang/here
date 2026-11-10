//! VirtIO random number generator (device ID 4).
//!
//! This is the Chapter 5 deliverable: a real system-level peripheral that Linux
//! recognises through its in-tree `virtio_rng` driver (`/dev/hwrng`).
//!
//! The device is a textbook "one queue, one writable buffer, no config space"
//! VirtIO device, which makes it small enough to read end to end while still
//! exercising every layer of the stack:
//!
//! ```text
//! guest: fill descriptor -> avail ring -> QueueNotify (doorbell)
//!   |
//!   v  (MMIO thread)
//! VirtIOMMIO::write_u32_impl(QueueNotify)
//!   -> fence(Release)                       publish the guest's ring writes
//!   -> VirtIORngDevice::notify()
//!        -> VirtQueue::take_one_request()   detach the descriptor chain
//!        -> mpsc::Sender::try_reserve()     非阻塞；队列满就返回 false
//!   |
//!   v  (TaskSpawner / Tokio worker, 独立线程)
//! read host entropy -> write into guest RAM via desc.paddr
//!   -> VirtQueueCompletion::complete(len)   写到 used ring
//!   -> isr.fetch_or(1, Ordering::Release)   发布：数据先于中断可见
//!   |
//!   v  (MMIO thread, next PLIC sample)
//! irq_level() => isr.load(Acquire) != 0 && queue.interrupts_enabled()
//!   -> PLIC -> mip.MEIP -> CPU trap
//! ```
//!
//! The `Release`/`Acquire` pair is what makes the guest see the random bytes
//! rather than a stale buffer: the worker's RAM writes are ordered before the
//! `isr` store, and the PLIC's `isr` load is ordered before the guest reads the
//! buffer.

use std::sync::{
    Arc,
    atomic::{AtomicU8, Ordering},
};

use log::{error, info};
use tokio::{fs::File, io::AsyncReadExt, sync::mpsc};

use crate::{
    device::virtio::{
        config::{VIRTQUEUE_MAX_SIZE, VirtIOFeatureSet, virtio_reserved_feature},
        virtio_device::{VirtIODeviceEnum, VirtIODeviceTrait},
        virtio_mmio::VirtIODeviceStatus,
        virtio_queue::{VirtQueue, VirtQueueCompletion, VirtQueueDesc},
    },
    task_spawner::{DeviceTask, TaskSpawner},
};

/// Host entropy source. `/dev/urandom` never blocks after boot, and it is
/// opened once at build time so the request path performs no `open(2)`.
pub(crate) const HOST_ENTROPY_PATH: &str = "/dev/urandom";

const RNG_TASK_CAPACITY: usize = VIRTQUEUE_MAX_SIZE as usize;

/// One detached request: where to write in guest RAM, and how to complete it.
struct RngRequest {
    /// Host pointer into the emulator's RAM, derived from `desc.paddr`.
    buf_ptr: usize,
    buf_len: usize,
    completion: VirtQueueCompletion,
    isr: Arc<AtomicU8>,
}

pub(crate) struct VirtIORngDevice {
    status: u8,
    // Shared with the worker: it is the only thing that ever *raises* the line.
    isr: Arc<AtomicU8>,
    host_feature: VirtIOFeatureSet,
    guest_feature: VirtIOFeatureSet,
    generation: u32,
    ram_base_raw: usize,
    queue: VirtQueue,
    sender: mpsc::Sender<RngRequest>,
}

// =====================================
//        Host-side async worker
// =====================================

pub(crate) struct VirtIORngWorker {
    file: File,
    receiver: mpsc::Receiver<RngRequest>,
}

impl VirtIORngWorker {
    fn new(file: File, receiver: mpsc::Receiver<RngRequest>) -> Self {
        Self { file, receiver }
    }

    /// Fill the guest buffer, then publish. Order matters and is the whole
    /// point of the exercise:
    ///
    /// 1. write the bytes into guest RAM (plain stores),
    /// 2. write the used ring (Release inside `insert_used`),
    /// 3. `isr.fetch_or(1, Release)` so the PLIC's `Acquire` load cannot be
    ///    observed before steps 1 and 2 are globally visible.
    async fn serve(&mut self, request: RngRequest) {
        // SAFETY: `VirtQueue::take_one_request` already validated that the
        // descriptor's [paddr, paddr+len) range is inside guest RAM, and the
        // guest must not recycle the buffer until it observes the used ring.
        let buf =
            unsafe { std::slice::from_raw_parts_mut(request.buf_ptr as *mut u8, request.buf_len) };

        let mut written = 0usize;
        while written < buf.len() {
            match self.file.read(&mut buf[written..]).await {
                Ok(0) => {
                    error!("virtio-rng: host entropy source hit EOF");
                    break;
                }
                Ok(n) => written += n,
                Err(err) => {
                    error!("virtio-rng: host entropy read failed: {err}");
                    break;
                }
            }
        }

        request.completion.complete(written as u32);
        request.isr.fetch_or(1, Ordering::Release);
    }
}

impl DeviceTask for VirtIORngWorker {
    async fn run_simple(mut self) {
        while let Some(request) = self.receiver.recv().await {
            self.serve(request).await;
        }
    }
}

// =====================================
//            The device itself
// =====================================

impl VirtIORngDevice {
    fn new(ram_base_raw: *mut u8, sender: mpsc::Sender<RngRequest>, generation: u32) -> Self {
        Self {
            status: 0,
            isr: Arc::new(AtomicU8::new(0)),
            // `virtio-rng` needs nothing beyond the mandatory feature.
            host_feature: virtio_reserved_feature::VERSION_1,
            guest_feature: 0,
            generation,
            ram_base_raw: ram_base_raw as usize,
            queue: VirtQueue::new(ram_base_raw, 0), // num is set by the driver later
            sender,
        }
    }
}

impl VirtIODeviceTrait for VirtIORngDevice {
    fn get_device_id(&self) -> u16 {
        VirtIODeviceEnum::VirtIORng as u16 // 4
    }

    fn status(&mut self) -> &mut u8 {
        &mut self.status
    }

    fn get_generation(&self) -> u32 {
        self.generation
    }

    fn reset(&mut self) {
        info!("virtio-rng: reset");
        self.status = 0;
        self.guest_feature = 0;
        self.queue.reset();
        self.isr.store(0, Ordering::Release);
    }

    fn isr(&self) -> &AtomicU8 {
        &self.isr
    }

    fn irq_level(&mut self) -> bool {
        // Acquire pairs with the worker's Release store in `serve`.
        self.isr.load(Ordering::Acquire) != 0 && self.queue.interrupts_enabled()
    }

    fn get_host_feature(&self) -> VirtIOFeatureSet {
        self.host_feature
    }

    fn set_feature(&mut self, feature: VirtIOFeatureSet) {
        // The transport ORs `FEATURES_OK` into the status register *before*
        // calling us; if the guest asked for something we cannot do, we are the
        // only one who can take it back.
        if feature & virtio_reserved_feature::VERSION_1 == 0
            || self.host_feature & feature != feature
        {
            error!("virtio-rng: unsupported feature set {feature:#x}, clearing FEATURES_OK");
            self.status &= !VirtIODeviceStatus::FEATURES_OK.bits();
        } else {
            self.guest_feature = feature;
        }
    }

    fn set_queue_num(&mut self, num: u32) {
        self.queue.set_queue_num(num);
    }

    fn queue_select(&self, _idx: u32) {
        // Only one queue.
    }

    fn set_desc(&mut self, addr: u64) {
        self.queue.set_desc(addr);
    }

    fn set_avail(&mut self, addr: u64) {
        self.queue.set_avail(addr);
    }

    fn set_used(&mut self, addr: u64) {
        self.queue.set_used(addr);
    }

    fn queue_ready(&self) -> bool {
        self.queue.ready()
    }

    fn get_num_of_queue(&self) -> u32 {
        1
    }

    /// Detach one available descriptor chain and hand it to the worker.
    ///
    /// Returns `false` when there is nothing left to do — either the avail ring
    /// is empty or the worker channel is full. Returning `false` on a full
    /// channel is what keeps this function non-blocking: the guest will simply
    /// ring the doorbell again later.
    fn manage_one_request(&mut self) -> bool {
        let Ok(permit) = self.sender.try_reserve() else {
            return false;
        };

        let ram_base_raw = self.ram_base_raw;
        let mut target: Option<(usize, usize)> = None;

        let Some(completion) = self.queue.take_one_request(|desc: &VirtQueueDesc, idx: usize| {
            // virtio-rng request = exactly one device-writable buffer. Writing
            // into a read-only descriptor would corrupt guest memory, so a
            // malformed chain is rejected rather than served.
            if idx == 0 && desc.device_writable() {
                target = Some((
                    desc.get_request_package::<u8>(ram_base_raw) as usize,
                    desc.len as usize,
                ));
            }
        }) else {
            return false;
        };

        let Some((buf_ptr, buf_len)) = target else {
            // Malformed chain: complete it with zero bytes instead of stalling
            // the guest forever.
            error!("virtio-rng: descriptor chain has no device-writable buffer");
            completion.complete(0);
            return true;
        };

        permit.send(RngRequest {
            buf_ptr,
            buf_len,
            completion,
            isr: self.isr.clone(),
        });
        true
    }

    fn notify(&mut self, _queue_idx: u32) {
        // Drain everything the guest queued; each iteration only moves a
        // pointer, all the real work happens on the TaskSpawner thread.
        while self.manage_one_request() {}
    }

    fn read_config(&mut self, _offset: u64, _len: u32) -> u64 {
        // VirtIO 1.0+ `virtio-rng` has no device-specific configuration space.
        0
    }

    fn write_config(&mut self, _offset: u64, _len: u32, _data: u64) {}
}

// =====================================
//              Builder
// =====================================

pub struct VirtIORngDeviceBuilder {
    device: VirtIORngDevice,
    receiver: mpsc::Receiver<RngRequest>,
    file: File,
}

impl VirtIORngDeviceBuilder {
    pub fn new(ram_base_raw: *mut u8) -> Self {
        let host_file = std::fs::File::open(HOST_ENTROPY_PATH)
            .unwrap_or_else(|err| panic!("virtio-rng: cannot open {HOST_ENTROPY_PATH}: {err}"));
        Self::from_host_file(ram_base_raw, host_file)
    }

    fn from_host_file(ram_base_raw: *mut u8, host_file: std::fs::File) -> Self {
        let (sender, receiver) = mpsc::channel(RNG_TASK_CAPACITY);
        Self {
            device: VirtIORngDevice::new(ram_base_raw, sender, 1),
            receiver,
            file: File::from_std(host_file),
        }
    }

    pub fn generation(mut self, generation: u32) -> Self {
        self.device.generation = generation;
        self
    }

    /// Hand the worker to the board's [`TaskSpawner`] and return the device.
    ///
    /// The device cannot register its own task: `TaskSpawner::start` consumes
    /// the spawner during `RVBoardBuilder::build`, so every task has to be
    /// known before that point.
    pub fn get_and_spawner_task(self, task_spawner: &mut TaskSpawner) -> VirtIORngDevice {
        let Self {
            device,
            receiver,
            file,
        } = self;
        task_spawner.register(VirtIORngWorker::new(file, receiver));
        device
    }

    #[cfg(test)]
    fn get_with_worker(self) -> (VirtIORngDevice, VirtIORngWorker) {
        let Self {
            device,
            receiver,
            file,
        } = self;
        (device, VirtIORngWorker::new(file, receiver))
    }
}

#[cfg(test)]
impl VirtIORngWorker {
    /// Run exactly one queued request to completion, synchronously. Lets the
    /// unit test observe the "submitted but not finished" state in between.
    pub(super) fn process_one(&mut self) -> bool {
        match self.receiver.try_recv() {
            Ok(request) => {
                tokio::runtime::Builder::new_current_thread()
                    .enable_all()
                    .build()
                    .unwrap()
                    .block_on(self.serve(request));
                true
            }
            Err(mpsc::error::TryRecvError::Empty | mpsc::error::TryRecvError::Disconnected) => false,
        }
    }
}

#[cfg(test)]
mod tests {
    use std::slice;

    use crate::{
        config::ram_config,
        device::virtio::virtio_queue::{
            VirtQueueAvail, VirtQueueAvailFlag, VirtQueueDescFlag, VirtQueueUsed, VirtQueueUsedFlag,
        },
        ram::Ram,
    };

    use super::*;

    const QUEUE_NUM: u32 = 8;
    const DESC_BASE: u64 = 0x8000_2000;
    const AVAIL_BASE: u64 = 0x8000_2100;
    const USED_BASE: u64 = 0x8000_2200;
    const BUF_ADDR: u64 = 0x8000_3000;
    const BUF_LEN: u32 = 32;
    const FILL_BYTE: u8 = 0x5a;

    fn ram_ptr(ram: &mut Ram, paddr: u64) -> *mut u8 {
        &mut ram[(paddr - ram_config::BASE_ADDR) as usize] as *mut u8
    }

    /// Build a board-less VirtIO-RNG whose entropy source is a file we control,
    /// so the assertion on the random bytes is deterministic.
    fn setup(name: &str) -> (Ram, VirtIORngDevice, VirtIORngWorker) {
        let mut ram = Ram::new();
        let ram_base = &mut ram[0] as *mut u8;

        // Each test gets its own source file: the harness runs them in parallel.
        let path = std::env::temp_dir().join(format!("here_virtio_rng_{name}.bin"));
        std::fs::write(&path, vec![FILL_BYTE; 256]).unwrap();
        let source = std::fs::File::open(&path).unwrap();
        let _ = std::fs::remove_file(&path);

        let (device, worker) =
            VirtIORngDeviceBuilder::from_host_file(ram_base, source).get_with_worker();
        (ram, device, worker)
    }

    fn init_queue(ram: &mut Ram, device: &mut VirtIORngDevice) {
        device.set_queue_num(QUEUE_NUM);
        device.set_desc(DESC_BASE);
        device.set_avail(AVAIL_BASE);
        device.set_used(USED_BASE);
        assert!(device.queue_ready(), "queue must be ready after setup");

        // Descriptor 0: a single device-writable buffer, no chaining.
        let desc = unsafe {
            slice::from_raw_parts_mut(
                ram_ptr(ram, DESC_BASE) as *mut VirtQueueDesc,
                QUEUE_NUM as usize,
            )
        };
        desc[0].init(BUF_ADDR, BUF_LEN, VirtQueueDescFlag::VIRTQ_DESC_F_WRITE, 0);

        // Available ring: one entry pointing at descriptor 0.
        let avail = unsafe { (ram_ptr(ram, AVAIL_BASE) as *mut VirtQueueAvail).as_mut().unwrap() };
        avail.init(VirtQueueAvailFlag::Default);
        avail.idx_atomic_add(1);
        // NOTE: `mut_ring` takes a **host** pointer, not a guest physical
        // address. Passing DESC/AVAIL/USED paddr here would fault the host.
        VirtQueueAvail::mut_ring(ram_ptr(ram, AVAIL_BASE) as u64, QUEUE_NUM)[0] = 0;

        // Used ring: empty.
        let used = unsafe { (ram_ptr(ram, USED_BASE) as *mut VirtQueueUsed).as_mut().unwrap() };
        used.init(VirtQueueUsedFlag::Default);
    }

    fn used_index(ram: &mut Ram) -> u16 {
        unsafe { (ram_ptr(ram, USED_BASE) as *mut VirtQueueUsed).as_mut().unwrap().get_index() }
    }

    #[test]
    fn serves_one_request_and_publishes_with_release_isr() {
        let (mut ram, mut device, mut worker) = setup("serve");
        init_queue(&mut ram, &mut device);

        assert_eq!(used_index(&mut ram), 0);
        assert!(!device.irq_level(), "idle device must not assert the line");

        // Doorbell: the request is now owned by the worker, unfinished.
        device.notify(0);
        assert_eq!(used_index(&mut ram), 0, "no completion before the worker runs");
        assert_eq!(device.isr().load(Ordering::Acquire), 0);
        assert!(!device.irq_level());

        // Worker completes it.
        assert!(worker.process_one());

        assert_eq!(used_index(&mut ram), 1);
        let used = unsafe { (ram_ptr(&mut ram, USED_BASE) as *mut VirtQueueUsed).as_mut().unwrap() };
        let elem = &used.ring(QUEUE_NUM)[0];
        assert_eq!(elem.get_id(), 0, "used ring must report the chain head");
        assert_eq!(elem.get_len(), BUF_LEN, "used ring must report bytes written");

        assert_eq!(device.isr().load(Ordering::Acquire), 1);
        assert!(device.irq_level(), "completed request must raise the line");

        // The guest buffer really received the entropy bytes.
        let buf = unsafe { slice::from_raw_parts(ram_ptr(&mut ram, BUF_ADDR), BUF_LEN as usize) };
        assert!(
            buf.iter().all(|b| *b == FILL_BYTE),
            "worker must write host entropy into guest RAM"
        );

        // Guest acks: the transport does `fetch_and(!mask, AcqRel)`.
        device.isr().fetch_and(!1, Ordering::AcqRel);
        assert!(!device.irq_level(), "ACK must lower the level");

        // Nothing left in the avail ring -> notify is a no-op.
        device.notify(0);
        assert!(!worker.process_one());
    }

    #[test]
    fn reset_clears_status_isr_and_queue() {
        let (mut ram, mut device, mut worker) = setup("reset");
        init_queue(&mut ram, &mut device);

        device.notify(0);
        assert!(worker.process_one());
        assert!(device.irq_level());

        device.reset();
        assert_eq!(*device.status(), 0);
        assert_eq!(device.isr().load(Ordering::Acquire), 0);
        assert!(!device.irq_level());
        assert!(!device.queue_ready(), "reset must invalidate the queue");
    }

    #[test]
    fn rejects_guest_features_beyond_host_capability() {
        let (_ram, mut device, _worker) = setup("features");
        assert_ne!(device.get_host_feature() & virtio_reserved_feature::VERSION_1, 0);

        // Feature negotiation is device-side responsible for rollback.
        *device.status() = VirtIODeviceStatus::FEATURES_OK.bits();
        device.set_feature(1 << 40); // host does not implement this
        assert_eq!(
            *device.status() & VirtIODeviceStatus::FEATURES_OK.bits(),
            0,
            "unsupported features must clear FEATURES_OK"
        );

        *device.status() = VirtIODeviceStatus::FEATURES_OK.bits();
        device.set_feature(virtio_reserved_feature::VERSION_1);
        assert_ne!(
            *device.status() & VirtIODeviceStatus::FEATURES_OK.bits(),
            0,
            "supported features must keep FEATURES_OK"
        );
    }
}
