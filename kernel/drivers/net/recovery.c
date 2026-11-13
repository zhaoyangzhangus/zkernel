#include <arch/x86_64/cpu.h>
#include <kernel/e1000.h>
#include <kernel/irq.h>
#include <kernel/pci.h>

#include "internal.h"

#define E1000_REG_ICR    0x00C0U
#define E1000_REG_IMS    0x00D0U
#define E1000_REG_IMC    0x00D8U
#define E1000_IMS_RXT0  (1U << 7)
#define E1000_IMS_RXDMT0 (1U << 4)
#define E1000_IMS_LSC   (1U << 2)
#define E1000_IRQ_VECTOR 0x50U

/* REFACTOR_P8_E1000_RECOVERY_OWNER：中断生命周期和延迟恢复。 */

static void e1000_recovery_irq_handler(uint8_t vector,
                                       struct arch_trap_frame *frame,
                                       void *argument) {
    e1000_recovery_context_t *context =
        (e1000_recovery_context_t *)argument;
    uint32_t causes;
    (void)vector;
    (void)frame;
    if (context == 0 || !context->irq_bound || context->read == 0) return;

    /* ICR 通过读取清除。ISR 只负责确认中断并将有界任务加入队列。 */
    causes = context->read(context->owner, E1000_REG_ICR);
    if (causes != 0U) (void)e1000_schedule_deferred_poll();
}

bool e1000_recovery_bind(e1000_recovery_context_t *context,
                         const struct pci_device *pci,
                         void *owner,
                         e1000_recovery_read_t read,
                         e1000_recovery_write_t write) {
    if (context == 0 || pci == 0 || owner == 0 || read == 0 || write == 0 ||
        pci->msi_capability == 0U ||
        E1000_IRQ_VECTOR < IRQ_VECTOR_FIRST ||
        E1000_IRQ_VECTOR > IRQ_VECTOR_LAST || context->irq_bound) {
        return false;
    }
    context->owner = owner;
    context->read = read;
    context->write = write;
    (void)context->read(context->owner, E1000_REG_ICR);
    if (irq_register(E1000_IRQ_VECTOR, e1000_recovery_irq_handler, context) != K_OK) {
        context->owner = 0;
        context->read = 0;
        context->write = 0;
        return false;
    }
    if (pci_msi_configure((pci_device_t *)pci, x86_current_apic_id(),
                          E1000_IRQ_VECTOR) != K_OK) {
        (void)irq_unregister(E1000_IRQ_VECTOR, e1000_recovery_irq_handler,
                             context);
        context->owner = 0;
        context->read = 0;
        context->write = 0;
        return false;
    }
    context->irq_vector = E1000_IRQ_VECTOR;
    context->irq_bound = true;
    context->write(context->owner, E1000_REG_IMS,
                   E1000_IMS_RXT0 | E1000_IMS_RXDMT0 | E1000_IMS_LSC);
    return true;
}

void e1000_recovery_unbind(e1000_recovery_context_t *context) {
    if (context == 0 || !context->irq_bound) return;
    if (context->write != 0) {
        context->write(context->owner, E1000_REG_IMC, UINT32_MAX);
    }
    if (context->read != 0) {
        (void)context->read(context->owner, E1000_REG_ICR);
    }
    (void)irq_unregister(context->irq_vector, e1000_recovery_irq_handler,
                         context);
    context->irq_vector = 0U;
    context->irq_bound = false;
    context->owner = 0;
    context->read = 0;
    context->write = 0;
}
