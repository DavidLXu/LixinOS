//! Polled TCP/IPv4 over QEMU virtio-net (MMIO bus 4).
use crate::{disk::Dma, monitor};
use alloc::{format, string::String, vec, vec::Vec};
use core::{cell::RefCell, net::Ipv4Addr, ptr::NonNull};
use smoltcp::{
    iface::{Config, Interface, SocketHandle, SocketSet},
    phy::{Device, DeviceCapabilities, Medium, RxToken, TxToken},
    socket::tcp::{self, State},
    time::{Duration, Instant},
    wire::{EthernetAddress, IpCidr},
};
use virtio_drivers::{
    device::net::VirtIONet,
    transport::{DeviceType, Transport, mmio::MmioTransport},
};

struct Nic {
    driver: VirtIONet<Dma, MmioTransport<'static>, 16>,
}
struct Rx(Vec<u8>);
struct Tx<'a>(&'a mut Nic);
impl RxToken for Rx {
    fn consume<R, F: FnOnce(&[u8]) -> R>(self, f: F) -> R {
        f(&self.0)
    }
}
impl TxToken for Tx<'_> {
    fn consume<R, F: FnOnce(&mut [u8]) -> R>(self, len: usize, f: F) -> R {
        let mut packet = self.0.driver.new_tx_buffer(len);
        let result = f(packet.packet_mut());
        // A failed transmission is packet loss; TCP retries after its timer.
        let _ = self.0.driver.send(packet);
        result
    }
}
impl Device for Nic {
    type RxToken<'a> = Rx;
    type TxToken<'a> = Tx<'a>;
    fn receive(&mut self, _: Instant) -> Option<(Rx, Tx<'_>)> {
        let packet = self.driver.receive().ok()?;
        let bytes = packet.packet().to_vec();
        self.driver.recycle_rx_buffer(packet).ok()?;
        Some((Rx(bytes), Tx(self)))
    }
    fn transmit(&mut self, _: Instant) -> Option<Tx<'_>> {
        self.driver.can_send().then_some(Tx(self))
    }
    fn capabilities(&self) -> DeviceCapabilities {
        let mut caps = DeviceCapabilities::default();
        caps.medium = Medium::Ethernet;
        caps.max_transmission_unit = 1514;
        caps
    }
}
struct Stack {
    nic: Nic,
    iface: Interface,
    sockets: SocketSet<'static>,
    closing: Vec<(SocketHandle, Instant)>,
    dns: SocketHandle,
    port: u16,
    last_poll: Instant,
}
const ADDRESS: Ipv4Addr = Ipv4Addr::new(10, 0, 2, 15);
const GATEWAY: Ipv4Addr = Ipv4Addr::new(10, 0, 2, 2);
fn now() -> Instant {
    Instant::from_millis((monitor::ticks() / (monitor::seconds_to_ticks(1) / 1000)) as i64)
}
// One CPU. No operation holding this borrow yields to another pane.
struct Network(RefCell<Option<Stack>>);
unsafe impl Sync for Network {}
static NETWORK: Network = Network(RefCell::new(None));

impl Stack {
    fn init() -> Option<Self> {
        let transport =
            unsafe { MmioTransport::new(NonNull::new(0x0a00_0800 as *mut _)?, 512).ok()? };
        if transport.device_type() != DeviceType::Network {
            return None;
        }
        let mut nic = Nic {
            driver: VirtIONet::new(transport, 2048).ok()?,
        };
        nic.driver.disable_interrupts();
        let mut config = Config::new(EthernetAddress(nic.driver.mac_address()).into());
        config.random_seed = monitor::ticks();
        let mut iface = Interface::new(config, &mut nic, now());
        iface.update_ip_addrs(|ips| {
            ips.push(IpCidr::new(ADDRESS.into(), 24)).unwrap();
        });
        iface.routes_mut().add_default_ipv4_route(GATEWAY).ok()?;
        let mut sockets = SocketSet::new(Vec::new());
        let dns = sockets.add(smoltcp::socket::dns::Socket::new(
            &[Ipv4Addr::new(10, 0, 2, 3).into()], Vec::new(),
        ));
        Some(Self {
            nic,
            iface,
            sockets, dns,
            closing: Vec::new(),
            port: 49152,
            last_poll: now(),
        })
    }
    fn poll(&mut self, time: Instant) {
        self.last_poll = time;
        self.iface.poll(time, &mut self.nic, &mut self.sockets);
        self.closing.retain(|&(handle, deadline)| {
            if self.sockets.get::<tcp::Socket>(handle).state() == State::Closed || time >= deadline
            {
                self.sockets.remove(handle);
                false
            } else {
                true
            }
        });
    }
    fn connect(&mut self, ip: Ipv4Addr, port: u16) -> Option<SocketHandle> {
        self.poll(now());
        // Closing sockets retain their buffers until FIN completes or its deadline.
        if self.sockets.iter().count() >= 128 {
            return None;
        }
        let mut socket = tcp::Socket::new(
            tcp::SocketBuffer::new(vec![0; 8192]),
            tcp::SocketBuffer::new(vec![0; 8192]),
        );
        let local = (0..16384).find_map(|_| {
            let p = self.port;
            self.port = self.port.checked_add(1).unwrap_or(49152);
            (!self.sockets.iter().any(|(_, s)| {
                let smoltcp::socket::Socket::Tcp(t) = s else { return false };
                t.local_endpoint().is_some_and(|e| e.port == p)
            }))
            .then_some(p)
        })?;
        socket
            .connect(self.iface.context(), (ip, port), local)
            .ok()?;
        let handle = self.sockets.add(socket);
        Some(handle)
    }
    fn close(&mut self, h: SocketHandle) {
        self.sockets.get_mut::<tcp::Socket>(h).close();
        self.closing.push((h, now() + Duration::from_secs(15)));
        self.poll(now());
    }
}
pub fn init() {
    *NETWORK.0.borrow_mut() = Stack::init();
}
pub fn idle_poll() {
    if let Some(net) = NETWORK.0.borrow_mut().as_mut() {
        let time = now();
        if time >= net.last_poll + Duration::from_millis(2) {
            net.poll(time);
        }
    }
}
pub fn info() -> Option<String> {
    let n = NETWORK.0.borrow();
    Some(format!(
        "eth0 virtio-net  MAC {}\nIPv4 {ADDRESS}/24  gateway {GATEWAY}\n",
        EthernetAddress(n.as_ref()?.nic.driver.mac_address())
    ))
}

// Keep the shared stack borrow inside each polling step. Other tasks can use
// their sockets while a DNS reply or a TCP handshake is outstanding.
pub(crate) fn resolve(name: &str) -> Option<Ipv4Addr> {
    use smoltcp::{
        socket::dns::{GetQueryResultError, Socket},
        wire::{DnsQueryType, IpAddress},
    };
    if let Ok(ip) = name.parse() {
        return Some(ip);
    }
    let query = {
        let mut network = NETWORK.0.borrow_mut();
        let net = network.as_mut()?;
        net.sockets
            .get_mut::<Socket>(net.dns)
            .start_query(net.iface.context(), name, DnsQueryType::A)
            .ok()?
    };
    let deadline = now() + Duration::from_secs(8);
    loop {
        {
            let mut network = NETWORK.0.borrow_mut();
            let net = network.as_mut()?;
            net.poll(now());
            let socket = net.sockets.get_mut::<Socket>(net.dns);
            match socket.get_query_result(query) {
                Ok(addrs) => {
                    let IpAddress::Ipv4(ip) = *addrs.first()?;
                    return Some(ip);
                }
                Err(GetQueryResultError::Failed) => return None,
                Err(GetQueryResultError::Pending) if now() >= deadline => {
                    socket.cancel_query(query);
                    return None;
                }
                _ => (),
            }
        }
        crate::scheduler::sleep_ms(2);
    }
}
fn connected(ip: Ipv4Addr, port: u16) -> Option<SocketHandle> {
    let handle = NETWORK.0.borrow_mut().as_mut()?.connect(ip, port)?;
    let deadline = now() + Duration::from_secs(5);
    loop {
        {
            let mut network = NETWORK.0.borrow_mut();
            let net = network.as_mut()?;
            net.poll(now());
            match net.sockets.get::<tcp::Socket>(handle).state() {
                State::Established | State::CloseWait => return Some(handle),
                State::SynSent if now() < deadline => (),
                _ => {
                    net.sockets.remove(handle);
                    return None;
                }
            }
        }
        crate::scheduler::sleep_ms(2);
    }
}

// Handles belong to the calling process. On exit or fault all its sockets are closed.
#[derive(Default)]
pub struct Connections([Option<SocketHandle>; 8]);
impl Connections {
    pub fn ready(&self,slot:usize)->Option<u8> {
        let h=(*self.0.get(slot)?)?;
        let mut network=NETWORK.0.borrow_mut();
        let net=network.as_mut()?;
        net.poll(now());
        let socket=net.sockets.get::<tcp::Socket>(h);
        Some(u8::from(socket.can_recv() || !socket.may_recv()) |
             (u8::from(socket.can_send() || !socket.may_send())<<1))
    }
    pub fn call(&mut self, op: u64, value: u64, mail: &mut [u8]) -> i64 {
        if op == 25 {
            let Some(slot) = self.0.iter().position(Option::is_none) else {
                return -1;
            };
            let Ok(port) = u16::try_from(value) else {
                return -1;
            };
            let addr = mail[..254]
                .split(|&b| b == 0)
                .next()
                .and_then(|bytes| core::str::from_utf8(bytes).ok())
                .and_then(resolve);
            let Some(h) = addr.and_then(|ip| connected(ip, port)) else {
                return -1;
            };
            self.0[slot] = Some(h);
            return slot as i64;
        }
        let mut network = NETWORK.0.borrow_mut();
        let Some(net) = network.as_mut() else {
            return -1;
        };
        let Some(slot) = self.0.get_mut((value >> 32) as usize) else {
            return -1;
        };
        let Some(h) = *slot else { return -1 };
        if op == 28 {
            net.close(h);
            *slot = None;
            return 0;
        }
        let Some(data) = mail.get_mut(..value as u32 as usize) else {
            return -1;
        };
        if data.is_empty() {
            return 0;
        }
        net.poll(now());
        let s = net.sockets.get_mut::<tcp::Socket>(h);
        if op == 26 {
            match s.recv_slice(data) {
                Ok(0) => -2,
                Ok(n) => n as i64,
                Err(tcp::RecvError::Finished) => 0,
                Err(_) => -1,
            }
        } else {
            match s.send_slice(data) {
                Ok(0) => -2,
                Ok(n) => {
                    net.poll(now());
                    n as i64
                }
                Err(_) => -1,
            }
        }
    }
}
impl Drop for Connections {
    fn drop(&mut self) {
        if let Some(net) = NETWORK.0.borrow_mut().as_mut() {
            for h in self.0.iter_mut().filter_map(Option::take) {
                net.close(h);
            }
        }
    }
}
