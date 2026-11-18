//! Host OS randomness from the QEMU virtio-rng on MMIO bus 5.
use crate::disk::Dma;
use core::{cell::RefCell, ptr::NonNull};
use virtio_drivers::{
    device::rng::VirtIORng,
    transport::{DeviceType, Transport, mmio::MmioTransport},
};

struct Source(RefCell<Option<VirtIORng<Dma, MmioTransport<'static>>>>);
// One vCPU; filling a buffer never switches panes.
unsafe impl Sync for Source {}
static SOURCE: Source = Source(RefCell::new(None));

pub fn init() {
    let header = NonNull::new(0x0a00_0a00 as *mut _).unwrap();
    if let Ok(t) = unsafe { MmioTransport::new(header, 512) } {
        if t.device_type() == DeviceType::EntropySource {
            *SOURCE.0.borrow_mut() = VirtIORng::new(t).ok();
        }
    }
}
pub fn fill(mut out: &mut [u8]) -> Option<()> {
    let mut source = SOURCE.0.borrow_mut();
    let rng = source.as_mut()?;
    // The mailbox is a kernel virtual alias of the active user bank. Dma::share
    // accepts only identity-mapped memory, so use a kernel stack buffer for DMA.
    let mut buffer = [0u8; 256];
    while !out.is_empty() {
        let count = out.len().min(buffer.len());
        let n = rng.request_entropy(&mut buffer[..count]).ok()?;
        if n == 0 || n > count {
            return None;
        }
        out[..n].copy_from_slice(&buffer[..n]);
        out = &mut out[n..];
    }
    Some(())
}
