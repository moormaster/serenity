/*
 * Copyright (c) 2021, Liav A. <liavalb@hotmail.co.il>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Kernel/CommandLine.h>
#include <Kernel/Storage/ATA/AHCIPortHandler.h>

namespace Kernel {

UNMAP_AFTER_INIT ErrorOr<NonnullRefPtr<AHCIPortHandler>> AHCIPortHandler::create(AHCIController& controller, u8 irq, AHCI::MaskedBitField taken_ports)
{
    auto port_handler = TRY(adopt_nonnull_ref_or_enomem(new (nothrow) AHCIPortHandler(controller, irq, taken_ports)));
    // FIXME: Propagate errors from this method too.
    port_handler->allocate_resources_and_initialize_ports();
    return port_handler;
}

void AHCIPortHandler::allocate_resources_and_initialize_ports()
{
    // FIXME: Use the number of taken ports to determine how many pages we should allocate.
    for (size_t index = 0; index < (((size_t)AHCI::Limits::MaxPorts * 512) / PAGE_SIZE); index++) {
        m_identify_metadata_pages.append(MM.allocate_supervisor_physical_page().release_value_but_fixme_should_propagate_errors());
    }

    // Clear pending interrupts, if there are any!
    m_pending_ports_interrupts.set_all();
    enable_irq();

    if (kernel_command_line().ahci_reset_mode() == AHCIResetMode::Aggressive) {
        for (auto index : m_taken_ports.to_vector()) {
            auto port = AHCIPort::create(*this, static_cast<volatile AHCI::PortRegisters&>(m_parent_controller->hba().port_regs[index]), index).release_value_but_fixme_should_propagate_errors();
            m_handled_ports[index] = port;
            port->reset();
        }
        return;
    }
    for (auto index : m_taken_ports.to_vector()) {
        auto port = AHCIPort::create(*this, static_cast<volatile AHCI::PortRegisters&>(m_parent_controller->hba().port_regs[index]), index).release_value_but_fixme_should_propagate_errors();
        m_handled_ports[index] = port;
        port->initialize_without_reset();
    }
}

UNMAP_AFTER_INIT AHCIPortHandler::AHCIPortHandler(AHCIController& controller, u8 irq, AHCI::MaskedBitField taken_ports)
    : IRQHandler(irq)
    , m_parent_controller(controller)
    , m_taken_ports(taken_ports)
    , m_pending_ports_interrupts(create_pending_ports_interrupts_bitfield())
{
    dbgln_if(AHCI_DEBUG, "AHCI Port Handler: IRQ {}", irq);
}

void AHCIPortHandler::enumerate_ports(Function<void(AHCIPort const&)> callback) const
{
    for (auto port : m_handled_ports) {
        if (port)
            callback(*port);
    }
}

RefPtr<AHCIPort> AHCIPortHandler::port_at_index(u32 port_index) const
{
    VERIFY(m_taken_ports.is_set_at(port_index));
    return m_handled_ports[port_index];
}

PhysicalAddress AHCIPortHandler::get_identify_metadata_physical_region(u32 port_index) const
{
    dbgln_if(AHCI_DEBUG, "AHCI Port Handler: Get identify metadata physical address of port {} - {}", port_index, (port_index * 512) / PAGE_SIZE);
    return m_identify_metadata_pages[(port_index * 512) / PAGE_SIZE].paddr().offset((port_index * 512) % PAGE_SIZE);
}

AHCI::MaskedBitField AHCIPortHandler::create_pending_ports_interrupts_bitfield() const
{
    return AHCI::MaskedBitField((u32 volatile&)m_parent_controller->hba().control_regs.is, m_taken_ports.bit_mask());
}

AHCI::HBADefinedCapabilities AHCIPortHandler::hba_capabilities() const
{
    return m_parent_controller->hba_capabilities();
}

AHCIPortHandler::~AHCIPortHandler() = default;

bool AHCIPortHandler::handle_irq(RegisterState const&)
{
    dbgln_if(AHCI_DEBUG, "AHCI Port Handler: IRQ received");
    if (m_pending_ports_interrupts.is_zeroed())
        return false;
    for (auto port_index : m_pending_ports_interrupts.to_vector()) {
        auto port = m_handled_ports[port_index];
        VERIFY(port);
        dbgln_if(AHCI_DEBUG, "AHCI Port Handler: Handling IRQ for port {}", port_index);
        port->handle_interrupt();
        // We do this to clear the pending interrupt after we handled it.
        m_pending_ports_interrupts.set_at(port_index);
    }
    return true;
}

}
