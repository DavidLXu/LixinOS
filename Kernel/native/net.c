#include "native.h"
#include "lwip/init.h"
#include "lwip/dns.h"
#include "lwip/tcp.h"
#include "lwip/timeouts.h"
#include "lwip/etharp.h"
#include "netif/ethernet.h"

#include "virtio.h"
enum { NET_HEADER = 12, MTU = 1500 };
static struct virtio nic = {(volatile u32 *)0x0a000800}, rng = {(volatile u32 *)0x0a000a00};
static struct queue rx, tx, entropy;
static u8 packets[Q][2048], transmit[2048], random_buffer[256];
static int entropy_ready, online;
static struct netif iface;
void net_assert(const char *message) {
    serial_text(message);
    power_off();
    for (;;) {
    }
}
unsigned sys_now(void) {
    u64 ticks = clock_ticks(), hz = clock_hz();
    return ticks / hz * 1000 + ticks % hz * 1000 / hz;
}
int entropy_read(u8 *out, unsigned n) {
    if (!entropy_ready)
        return -1;
    while (n) {
        unsigned size = n > sizeof(random_buffer) ? sizeof(random_buffer) : n;
        submit(&entropy, 0, random_buffer, size, 1);
        rng.r[0x50 / 4] = 0;
        while (entropy.seen == entropy.used.index) {
        }
        BARRIER();
        u32 i = entropy.seen++ % Q, got = entropy.used.ring[i].len;
        if (entropy.used.ring[i].id || !got || got > size)
            return -1;
        memcpy(out, random_buffer, got);
        out += got;
        n -= got;
    }
    return 0;
}
unsigned net_random(void) {
    unsigned n;
    if (entropy_read((u8 *)&n, sizeof(n)) < 0)
        net_assert("virtio-rng unavailable");
    return n;
}
static err_t output(struct netif *link, struct pbuf *p) {
    (void)link;
    if (p->tot_len > sizeof(transmit) - NET_HEADER)
        return ERR_BUF;
    memset(transmit, 0, NET_HEADER);
    pbuf_copy_partial(p, transmit + NET_HEADER, p->tot_len, 0);
    submit(&tx, 0, transmit, p->tot_len + NET_HEADER, 0);
    nic.r[0x50 / 4] = 1;
    while (tx.seen == tx.used.index) {
    }
    BARRIER();
    unsigned id = tx.used.ring[tx.seen++ % Q].id;
    return id == 0 ? ERR_OK : ERR_IF;
}
static err_t init_link(struct netif *link) {
    link->name[0] = 'e';
    link->name[1] = 'n';
    link->hwaddr_len = 6;
    link->mtu = MTU;
    for (unsigned i = 0; i < 6; i++)
        link->hwaddr[i] = ((volatile u8 *)nic.r)[0x100 + i];
    link->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP;
    link->output = etharp_output;
    link->linkoutput = output;
    return ERR_OK;
}
void net_init(void) {
    if (device(rng, 4, 0) && setup_queue(rng, 0, &entropy)) {
        rng.r[0x70 / 4] = 15;
        entropy_ready = 1;
    }
    if (!entropy_ready || !device(nic, 1, 1u << 5) || !setup_queue(nic, 0, &rx) ||
        !setup_queue(nic, 1, &tx))
        return;
    for (unsigned i = 0; i < Q; i++)
        submit(&rx, i, packets[i], sizeof(packets[i]), 1);
    nic.r[0x70 / 4] = 15;
    nic.r[0x50 / 4] = 0;
    ip4_addr_t ip, mask, gw, ns;
    IP4_ADDR(&ip, 10, 0, 2, 15);
    IP4_ADDR(&mask, 255, 255, 255, 0);
    IP4_ADDR(&gw, 10, 0, 2, 2);
    IP4_ADDR(&ns, 10, 0, 2, 3);
    lwip_init();
    if (!netif_add(&iface, &ip, &mask, &gw, 0, init_link, ethernet_input))
        return;
    netif_set_default(&iface);
    netif_set_up(&iface);
    dns_setserver(0, &ns);
    online = 1;
}
void net_poll(void) {
    if (!online)
        return;
    while (rx.seen != rx.used.index) {
        BARRIER();
        unsigned slot = rx.seen++ % Q, id = rx.used.ring[slot].id, size = rx.used.ring[slot].len;
        if (id >= Q) {
            online = 0;
            return;
        }
        if (size > NET_HEADER && size <= sizeof(packets[id])) {
            struct pbuf *p = pbuf_alloc(PBUF_RAW, size - NET_HEADER, PBUF_POOL);
            if (p && (pbuf_take(p, packets[id] + NET_HEADER, size - NET_HEADER) != ERR_OK ||
                      iface.input(p, &iface) != ERR_OK))
                pbuf_free(p);
        }
        submit(&rx, id, packets[id], sizeof(packets[id]), 1);
        nic.r[0x50 / 4] = 0;
    }
    sys_check_timeouts();
}
long net_info(char *out) {
    if (!online)
        return -1;
    char text[] = "eth0 virtio-net  MAC 00:00:00:00:00:00\nIPv4 10.0.2.15/24  gateway 10.0.2.2\n";
    const char *hex = "0123456789abcdef";
    for (unsigned i = 0; i < 6; i++) {
        text[21 + i * 3] = hex[iface.hwaddr[i] >> 4];
        text[22 + i * 3] = hex[iface.hwaddr[i] & 15];
    }
    memcpy(out, text, sizeof(text));
    return sizeof(text) - 1;
}

struct connection {
    struct tcp_pcb *pcb;
    int taken, ready, eof, failed;
    u32 head, tail;
    u8 data[16384];
};
static struct connection connections[64];
static void failed(void *arg, err_t e) {
    (void)e;
    struct connection *c = arg;
    c->failed = 1;
    c->pcb = 0;
}
static err_t connected(void *arg, struct tcp_pcb *pcb, err_t err) {
    (void)pcb;
    struct connection *c = arg;
    c->ready = err == ERR_OK;
    c->failed = err != ERR_OK;
    return ERR_OK;
}
static err_t receive(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    (void)pcb;
    (void)err;
    struct connection *c = arg;
    if (!p) {
        c->eof = 1;
        return ERR_OK;
    }
    if (p->tot_len > sizeof(c->data) - (c->tail - c->head))
        return ERR_MEM;
    unsigned offset = c->tail % sizeof(c->data), n = sizeof(c->data) - offset;
    if (n > p->tot_len)
        n = p->tot_len;
    pbuf_copy_partial(p, c->data + offset, n, 0);
    pbuf_copy_partial(p, c->data, p->tot_len - n, n);
    c->tail += p->tot_len;
    pbuf_free(p);
    return ERR_OK;
}
struct answer {
    ip_addr_t addr;
    int done;
};
static void resolved(const char *name, const ip_addr_t *addr, void *arg) {
    (void)name;
    struct answer *a = arg;
    if (addr)
        a->addr = *addr;
    a->done = addr ? 1 : -1;
}
void socket_release(int id) {
    struct connection *c = &connections[id];
    if (c->pcb) {
        tcp_arg(c->pcb, 0);
        tcp_err(c->pcb, 0);
        tcp_recv(c->pcb, 0);
        if (!c->ready || tcp_close(c->pcb) != ERR_OK)
            tcp_abort(c->pcb);
    }
    c->taken = 0;
}
static int lookup_ipv4_name(const char *name, ip_addr_t *ip) {
    if (!online)
        return -1;
    struct answer a = {0};
    err_t rc = dns_gethostbyname(name, &a.addr, resolved, &a);
    if (rc == ERR_INPROGRESS) {
        while (!a.done) {
            net_poll();
            schedule();
        }
        if (a.done < 0)
            return -1;
    } else if (rc != ERR_OK)
        return -1;
    *ip = a.addr;
    return 0;
}
long socket_resolve(const char *name) {
    ip_addr_t ip;
    return lookup_ipv4_name(name, &ip) < 0 ? -1L : (long)lwip_ntohl(ip4_addr_get_u32(ip_2_ip4(&ip)));
}
int socket_connect(const char *name, unsigned port) {
    ip_addr_t ip;
    if (!port || port > 65535 || lookup_ipv4_name(name, &ip) < 0) return -1;
    unsigned i = 0;
    while (i < 64 && connections[i].taken)
        i++;
    if (i == 64)
        return -1;
    struct connection *c = &connections[i];
    memset(c, 0, sizeof(*c));
    c->pcb = tcp_new();
    if (!c->pcb)
        return -1;
    c->taken = 1;
    tcp_arg(c->pcb, c);
    tcp_err(c->pcb, failed);
    tcp_recv(c->pcb, receive);
    unsigned start = sys_now();
    if (tcp_connect(c->pcb, &ip, port, connected) == ERR_OK) {
        while (!c->ready && !c->failed && sys_now() - start < 10000) {
            net_poll();
            schedule();
        }
    }
    if (c->ready && !c->failed)
        return i;
    socket_release(i);
    return -1;
}
long socket_io(int id, int write, u8 *bytes, unsigned size) {
    net_poll();
    struct connection *c = &connections[id];
    if (!size)
        return 0;
    if (write) {
        if (!c->pcb || c->failed)
            return -1;
        unsigned n = tcp_sndbuf(c->pcb);
        if (n > size)
            n = size;
        if (!n)
            return -2;
        err_t e = tcp_write(c->pcb, bytes, n, TCP_WRITE_FLAG_COPY);
        if (e != ERR_OK)
            return e == ERR_MEM ? -2 : -1;
        tcp_output(c->pcb);
        return n;
    }
    unsigned n = c->tail - c->head;
    if (!n)
        return c->failed ? -1 : c->eof ? 0 : -2;
    if (n > size)
        n = size;
    for (unsigned i = 0; i < n; i++)
        bytes[i] = c->data[c->head++ % sizeof(c->data)];
    if (c->pcb)
        tcp_recved(c->pcb, n);
    return n;
}
unsigned socket_ready(int id) {
    net_poll();
    struct connection *c = &connections[id];
    return (c->tail != c->head || c->eof || c->failed) |
           ((c->failed || (c->pcb && tcp_sndbuf(c->pcb))) ? 2 : 0);
}
