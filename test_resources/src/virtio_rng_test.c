// virtio_rng_test.c —— Chapter 5 交付物验证：VirtIO-RNG（Device ID 4）
//
// 这是 §七 要求的完整体验链路：
//
//   guest 填描述符 → avail ring → 敲 QueueNotify（门铃）
//     → transport fence(Release) → 设备 notify()
//     → 请求交给 TaskSpawner 后台线程（非阻塞返回）
//     → worker 从宿主 /dev/urandom 读数据，直接写 guest RAM
//     → 填 used ring → isr.fetch_or(1, Release)
//     → PLIC 采样 irq_level()（Acquire）→ MEIP → trap
//     → guest: claim → 校验 → InterruptAck → complete
//
// 注意 used ring 和 isr 是异步产生的，所以这里用中断而不是立即读，
// 既贴近真实驱动，也避免「恰好比 worker 快」的偶然通过。

#include "io.h"
#include "log.h"
#include "trap.h"
#include "virtio/virtio_mmio.h"
#include "virtio/virtio_queue.h"
#include <stdint.h>

#define QUEUE_SIZE 8
#define RNG_BUF_SIZE 64

// ---- VirtIO MMIO transport ----
#define VIRTIO_MMIO_BASE 0x10001000UL
#define VIRTIO_DEVICE_ID_RNG 4

#define VIO(off) (*(volatile uint32_t *)(VIRTIO_MMIO_BASE + (off)))
#define R_MAGIC 0x000
#define R_VERSION 0x004
#define R_DEVICE_ID 0x008
#define R_DEV_FEAT 0x010
#define R_DEV_FEAT_SEL 0x014
#define R_DRV_FEAT 0x020
#define R_DRV_FEAT_SEL 0x024
#define R_Q_SEL 0x030
#define R_Q_NUM_MAX 0x034
#define R_Q_NUM 0x038
#define R_Q_READY 0x044
#define R_Q_NOTIFY 0x050
#define R_INT_STATUS 0x060
#define R_INT_ACK 0x064
#define R_STATUS 0x070
#define R_Q_DESC_LO 0x080
#define R_Q_DESC_HI 0x084
#define R_Q_AVAIL_LO 0x090
#define R_Q_AVAIL_HI 0x094
#define R_Q_USED_LO 0x0A0
#define R_Q_USED_HI 0x0A4

#define VIRTQ_MAGIC 0x74726976U
#define VIRTQ_VERSION 0x2U
#define FEATURE_WORD_0 0U
#define FEATURE_WORD_1 1U
#define VIRTIO_F_VERSION_1_BIT 32U // bit 32 => word 1, bit 0

// ---- PLIC，hart0 machine mode == context 0 ----
#define PLIC_BASE 0xc000000UL
#define PLIC_PRIORITY(id) (PLIC_BASE + 4 * (id))
#define PLIC_ENABLE(ctx, id) \
    (PLIC_BASE + 0x2000 + (ctx) * 0x80 + 4 * ((id) / 32))
#define PLIC_THRESHOLD(ctx) (PLIC_BASE + 0x200000 + (ctx) * 0x1000)
#define PLIC_CLAIM(ctx) (PLIC_BASE + 0x200004 + (ctx) * 0x1000)
#define M_CONTEXT 0
#define VIRTIO_IRQ 1 // VIRTIO_IRQ_BASE
#define MCAUSE_MACHINE_EXTERNAL ((1ull << 63) | 11)

static inline void mmio_w32(uint64_t addr, uint32_t val) {
    *(volatile uint32_t *)addr = val;
}
static inline uint32_t mmio_r32(uint64_t addr) {
    return *(volatile uint32_t *)addr;
}

// ---- 队列内存（物理地址 == 虚拟地址，裸机无 MMU）----
#define AVAIL_RING_SIZE (sizeof(uint16_t) * QUEUE_SIZE + sizeof(VirtQueueAvail) + 4)
#define USED_RING_SIZE (sizeof(VirtQueueUsedElem) * QUEUE_SIZE + sizeof(VirtQueueUsed) + 4)

static VirtQueueDesc desc[QUEUE_SIZE] __attribute__((aligned(64)));
static uint8_t avail_mem[AVAIL_RING_SIZE] __attribute__((aligned(64)));
static uint8_t used_mem[USED_RING_SIZE] __attribute__((aligned(64)));
static VirtQueueAvail *avail = (VirtQueueAvail *)avail_mem;
static VirtQueueUsed *used = (VirtQueueUsed *)used_mem;
static uint8_t rng_buf[RNG_BUF_SIZE] __attribute__((aligned(64)));

static volatile uint32_t irq_count = 0;
static volatile uint32_t seen_irq_id = 0xffffffff;
static volatile uint16_t seen_used_idx = 0;

void trap_handler(TrapContext *trap_ctx) {
    uint64_t mcause = read_csr_mcause();
    if (mcause != MCAUSE_MACHINE_EXTERNAL) {
        Log(ERROR, "unexpected trap: mcause=%x mtval=%x", mcause, read_csr_mtval());
        fail();
    }

    uint32_t irq = mmio_r32(PLIC_CLAIM(M_CONTEXT));
    seen_irq_id = irq;

    if (irq == VIRTIO_IRQ) {
        seen_used_idx = used->idx;
        // 清 isr 才能让电平降下来，否则 PLIC 会立刻重复触发。
        VIO(R_INT_ACK) = VIRTIO_MMIO_INT_VRING;
        irq_count++;
    }

    mmio_w32(PLIC_CLAIM(M_CONTEXT), irq);
    __traps_return(trap_ctx);
}

/// 提交一个「请给我 RNG_BUF_SIZE 字节随机数」的请求。
static void submit_request(void) {
    uint16_t next_idx = avail->idx;

    desc[0].paddr = (uint64_t)rng_buf;
    desc[0].len = RNG_BUF_SIZE;
    desc[0].flags = VIRTQ_DESC_F_WRITE; // 设备只写
    desc[0].next = 0;

    avail->ring[next_idx % QUEUE_SIZE] = 0; // 描述符链头
    avail->idx = next_idx + 1;

    // 门铃
    VIO(R_Q_NOTIFY) = 0;
}

static void wait_for_interrupt(const char *what) {
    uint32_t before = irq_count;
    for (uint64_t watchdog = 0; irq_count == before; watchdog++) {
        if (watchdog > 200000000ull) {
            Log(ERROR, "%s: virtio interrupt never arrived", what);
            fail();
        }
    }
}

int main(void) {
    TEST_START(__BASE_FILE__);
    trap_init();

    // ---- 1. 发现设备 ----
    if (VIO(R_MAGIC) != VIRTQ_MAGIC) {
        Log(ERROR, "bad magic: %x", VIO(R_MAGIC));
        fail();
    }
    if (VIO(R_VERSION) != VIRTQ_VERSION) {
        Log(ERROR, "unsupported version: %d", VIO(R_VERSION));
        fail();
    }
    if (VIO(R_DEVICE_ID) != VIRTIO_DEVICE_ID_RNG) {
        Log(ERROR, "unexpected device id: %d (want 4)", VIO(R_DEVICE_ID));
        fail();
    }
    Log(INFO, "virtio-rng found (version %d)", VIO(R_VERSION));

    // ---- 2. 复位 + 状态机 ----
    VIO(R_STATUS) = 0;
    VIO(R_STATUS) = ACKNOWLEDGE;
    VIO(R_STATUS) = ACKNOWLEDGE | DRIVER;

    // ---- 3. feature 协商：必须接受 VERSION_1 才能用 modern MMIO ----
    VIO(R_DEV_FEAT_SEL) = FEATURE_WORD_1;
    if ((VIO(R_DEV_FEAT) & (1U << (VIRTIO_F_VERSION_1_BIT - 32))) == 0) {
        Log(ERROR, "host does not advertise VIRTIO_F_VERSION_1");
        fail();
    }
    VIO(R_DRV_FEAT_SEL) = FEATURE_WORD_1;
    VIO(R_DRV_FEAT) = 1U << (VIRTIO_F_VERSION_1_BIT - 32);
    VIO(R_DRV_FEAT_SEL) = FEATURE_WORD_0;
    VIO(R_DRV_FEAT) = 0;

    VIO(R_STATUS) = ACKNOWLEDGE | DRIVER | FEATURES_OK;
    if (!(VIO(R_STATUS) & FEATURES_OK)) {
        Log(ERROR, "feature negotiation rejected by device");
        fail();
    }

    // ---- 4. 队列初始化 ----
    VIO(R_Q_SEL) = 0;
    if (VIO(R_Q_NUM_MAX) == 0) {
        Log(ERROR, "queue 0 is not available");
        fail();
    }
    VIO(R_Q_NUM) = QUEUE_SIZE;
    VIO(R_Q_DESC_LO) = (uint32_t)(uint64_t)desc;
    VIO(R_Q_DESC_HI) = (uint32_t)((uint64_t)desc >> 32);
    VIO(R_Q_AVAIL_LO) = (uint32_t)(uint64_t)avail_mem;
    VIO(R_Q_AVAIL_HI) = (uint32_t)((uint64_t)avail_mem >> 32);
    VIO(R_Q_USED_LO) = (uint32_t)(uint64_t)used_mem;
    VIO(R_Q_USED_HI) = (uint32_t)((uint64_t)used_mem >> 32);
    VIO(R_Q_READY) = 1;

    avail->flags = AvailFlagDefault;
    avail->idx = 0;
    used->flags = UsedFlagDefault;
    used->idx = 0;

    // ---- 5. 打开 PLIC 通路 ----
    mmio_w32(PLIC_PRIORITY(VIRTIO_IRQ), 1);
    mmio_w32(PLIC_THRESHOLD(M_CONTEXT), 0);
    mmio_w32(PLIC_ENABLE(M_CONTEXT, VIRTIO_IRQ),
             mmio_r32(PLIC_ENABLE(M_CONTEXT, VIRTIO_IRQ)) |
                 (1u << (VIRTIO_IRQ % 32)));

    VIO(R_STATUS) = ACKNOWLEDGE | DRIVER | FEATURES_OK | DRIVER_OK;

    // ---- 6. 第一个事务 ----
    for (int i = 0; i < RNG_BUF_SIZE; i++) {
        rng_buf[i] = 0;
    }
    submit_request();
    wait_for_interrupt("first request");

    if (seen_irq_id != VIRTIO_IRQ) {
        Log(ERROR, "claimed wrong source: %d", seen_irq_id);
        fail();
    }
    if (used->idx != 1) {
        Log(ERROR, "used->idx = %d, want 1", used->idx);
        fail();
    }
    if (used->ring[0].id != 0) {
        Log(ERROR, "used ring head = %d, want 0", used->ring[0].id);
        fail();
    }
    if (used->ring[0].len != RNG_BUF_SIZE) {
        Log(ERROR, "used ring len = %d, want %d", used->ring[0].len, RNG_BUF_SIZE);
        fail();
    }

    int nonzero = 0;
    for (int i = 0; i < RNG_BUF_SIZE; i++) {
        nonzero += (rng_buf[i] != 0);
    }
    if (nonzero < RNG_BUF_SIZE / 2) {
        Log(ERROR, "RNG output looks wrong: only %d/%d bytes non-zero", nonzero,
            RNG_BUF_SIZE);
        fail();
    }

    Log(INFO, "first 8 bytes: %x %x %x %x %x %x %x %x", rng_buf[0], rng_buf[1],
        rng_buf[2], rng_buf[3], rng_buf[4], rng_buf[5], rng_buf[6], rng_buf[7]);

    // ---- 7. 第二个事务：证明队列能循环使用 ----
    for (int i = 0; i < RNG_BUF_SIZE; i++) {
        rng_buf[i] = 0;
    }
    submit_request();
    wait_for_interrupt("second request");

    if (used->idx != 2) {
        Log(ERROR, "used->idx = %d, want 2", used->idx);
        fail();
    }
    if (used->ring[1].id != 0) {
        Log(ERROR, "second used ring head = %d, want 0", used->ring[1].id);
        fail();
    }

    // ACK 之后中断状态必须清零（电平已降）
    if (VIO(R_INT_STATUS) != 0) {
        Log(ERROR, "interrupt status not cleared after ACK: %x", VIO(R_INT_STATUS));
        fail();
    }

    Log(INFO, "virtio-rng ok: %d requests served", (int)irq_count);
    pass();
    return 0;
}
