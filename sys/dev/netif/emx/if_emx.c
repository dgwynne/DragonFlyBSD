/*
 * Copyright (c) 2004 Joerg Sonnenberger <joerg@bec.de>.  All rights reserved.
 *
 * Copyright (c) 2001-2008, Intel Corporation
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  1. Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *
 *  3. Neither the name of the Intel Corporation nor the names of its
 *     contributors may be used to endorse or promote products derived from
 *     this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 *
 * Copyright (c) 2005 The DragonFly Project.  All rights reserved.
 *
 * This code is derived from software contributed to The DragonFly Project
 * by Matthew Dillon <dillon@backplane.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name of The DragonFly Project nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific, prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "opt_polling.h"
#include "opt_serializer.h"
#include "opt_rss.h"
#include "opt_emx.h"

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/endian.h>
#include <sys/interrupt.h>
#include <sys/kernel.h>
#include <sys/ktr.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/proc.h>
#include <sys/rman.h>
#include <sys/serialize.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

#include <net/bpf.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <net/if_dl.h>
#include <net/if_media.h>
#include <net/ifq_var.h>
#include <net/vlan/if_vlan_var.h>
#include <net/vlan/if_vlan_ether.h>

#include <netinet/in_systm.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <netinet/udp.h>

#include <bus/pci/pcivar.h>
#include <bus/pci/pcireg.h>

#include <dev/netif/ig_hal/e1000_api.h>
#include <dev/netif/ig_hal/e1000_82571.h>
#include <dev/netif/emx/if_emx.h>

#ifdef EMX_RSS_DEBUG
#define EMX_RSS_DPRINTF(sc, lvl, fmt, ...) \
do { \
	if (sc->rss_debug > lvl) \
		if_printf(&sc->arpcom.ac_if, fmt, __VA_ARGS__); \
} while (0)
#else	/* !EMX_RSS_DEBUG */
#define EMX_RSS_DPRINTF(sc, lvl, fmt, ...)	((void)0)
#endif	/* EMX_RSS_DEBUG */

#define EMX_NAME	"Intel(R) PRO/1000 "

#define EMX_DEVICE(id)	\
	{ EMX_VENDOR_ID, E1000_DEV_ID_##id, EMX_NAME #id }
#define EMX_DEVICE_NULL	{ 0, 0, NULL }

static const struct emx_device {
	uint16_t	vid;
	uint16_t	did;
	const char	*desc;
} emx_devices[] = {
	EMX_DEVICE(82571EB_COPPER),
	EMX_DEVICE(82571EB_FIBER),
	EMX_DEVICE(82571EB_SERDES),
	EMX_DEVICE(82571EB_SERDES_DUAL),
	EMX_DEVICE(82571EB_SERDES_QUAD),
	EMX_DEVICE(82571EB_QUAD_COPPER),
	EMX_DEVICE(82571EB_QUAD_COPPER_LP),
	EMX_DEVICE(82571EB_QUAD_FIBER),
	EMX_DEVICE(82571PT_QUAD_COPPER),

	EMX_DEVICE(82572EI_COPPER),
	EMX_DEVICE(82572EI_FIBER),
	EMX_DEVICE(82572EI_SERDES),
	EMX_DEVICE(82572EI),

	EMX_DEVICE(82573E),
	EMX_DEVICE(82573E_IAMT),
	EMX_DEVICE(82573L),

	EMX_DEVICE(80003ES2LAN_COPPER_SPT),
	EMX_DEVICE(80003ES2LAN_SERDES_SPT),
	EMX_DEVICE(80003ES2LAN_COPPER_DPT),
	EMX_DEVICE(80003ES2LAN_SERDES_DPT),

	EMX_DEVICE(82574L),

	/* required last entry */
	EMX_DEVICE_NULL
};

static int	emx_probe(device_t);
static int	emx_attach(device_t);
static int	emx_detach(device_t);
static int	emx_shutdown(device_t);
static int	emx_suspend(device_t);
static int	emx_resume(device_t);

static void	emx_init(void *);
static void	emx_stop(struct emx_softc *);
static int	emx_ioctl(struct ifnet *, u_long, caddr_t, struct ucred *);
static void	emx_start(struct ifnet *);
#ifdef DEVICE_POLLING
static void	emx_poll(struct ifnet *, enum poll_cmd, int);
#endif
static void	emx_watchdog(struct ifnet *);
static void	emx_media_status(struct ifnet *, struct ifmediareq *);
static int	emx_media_change(struct ifnet *);
static void	emx_timer(void *);

static void	emx_intr(void *);
static void	emx_rxeof(struct emx_softc *, int, int);
static void	emx_txeof(struct emx_softc *);
static void	emx_tx_collect(struct emx_softc *);
static void	emx_tx_purge(struct emx_softc *);
static void	emx_enable_intr(struct emx_softc *);
static void	emx_disable_intr(struct emx_softc *);

static int	emx_dma_alloc(struct emx_softc *);
static void	emx_dma_free(struct emx_softc *);
static void	emx_init_tx_ring(struct emx_softc *);
static int	emx_init_rx_ring(struct emx_softc *, struct emx_rxdata *);
static void	emx_free_rx_ring(struct emx_softc *, struct emx_rxdata *);
static int	emx_create_tx_ring(struct emx_softc *);
static int	emx_create_rx_ring(struct emx_softc *, struct emx_rxdata *);
static void	emx_destroy_tx_ring(struct emx_softc *, int);
static void	emx_destroy_rx_ring(struct emx_softc *,
		    struct emx_rxdata *, int);
static int	emx_newbuf(struct emx_softc *, struct emx_rxdata *, int, int);
static int	emx_encap(struct emx_softc *, struct mbuf **);
static int	emx_txcsum_pullup(struct emx_softc *, struct mbuf **);
static int	emx_txcsum(struct emx_softc *, struct mbuf *,
		    uint32_t *, uint32_t *);

static int 	emx_is_valid_eaddr(const uint8_t *);
static int	emx_hw_init(struct emx_softc *);
static void	emx_setup_ifp(struct emx_softc *);
static void	emx_init_tx_unit(struct emx_softc *);
static void	emx_init_rx_unit(struct emx_softc *);
static void	emx_update_stats(struct emx_softc *);
static void	emx_set_promisc(struct emx_softc *);
static void	emx_disable_promisc(struct emx_softc *);
static void	emx_set_multi(struct emx_softc *);
static void	emx_update_link_status(struct emx_softc *);
static void	emx_smartspeed(struct emx_softc *);

static void	emx_print_debug_info(struct emx_softc *);
static void	emx_print_nvm_info(struct emx_softc *);
static void	emx_print_hw_stats(struct emx_softc *);

static int	emx_sysctl_stats(SYSCTL_HANDLER_ARGS);
static int	emx_sysctl_debug_info(SYSCTL_HANDLER_ARGS);
static int	emx_sysctl_int_throttle(SYSCTL_HANDLER_ARGS);
static int	emx_sysctl_int_tx_nsegs(SYSCTL_HANDLER_ARGS);
static void	emx_add_sysctl(struct emx_softc *);

/* Management and WOL Support */
static void	emx_get_mgmt(struct emx_softc *);
static void	emx_rel_mgmt(struct emx_softc *);
static void	emx_get_hw_control(struct emx_softc *);
static void	emx_rel_hw_control(struct emx_softc *);
static void	emx_enable_wol(device_t);

static device_method_t emx_methods[] = {
	/* Device interface */
	DEVMETHOD(device_probe,		emx_probe),
	DEVMETHOD(device_attach,	emx_attach),
	DEVMETHOD(device_detach,	emx_detach),
	DEVMETHOD(device_shutdown,	emx_shutdown),
	DEVMETHOD(device_suspend,	emx_suspend),
	DEVMETHOD(device_resume,	emx_resume),
	{ 0, 0 }
};

static driver_t emx_driver = {
	"emx",
	emx_methods,
	sizeof(struct emx_softc),
};

static devclass_t emx_devclass;

DECLARE_DUMMY_MODULE(if_emx);
MODULE_DEPEND(emx, ig_hal, 1, 1, 1);
DRIVER_MODULE(if_emx, pci, emx_driver, emx_devclass, 0, 0);

/*
 * Tunables
 */
static int	emx_int_throttle_ceil = EMX_DEFAULT_ITR;
static int	emx_rxd = EMX_DEFAULT_RXD;
static int	emx_txd = EMX_DEFAULT_TXD;
static int	emx_smart_pwr_down = FALSE;

/* Controls whether promiscuous also shows bad packets */
static int	emx_debug_sbp = FALSE;

static int	emx_82573_workaround = TRUE;

TUNABLE_INT("hw.emx.int_throttle_ceil", &emx_int_throttle_ceil);
TUNABLE_INT("hw.emx.rxd", &emx_rxd);
TUNABLE_INT("hw.emx.txd", &emx_txd);
TUNABLE_INT("hw.emx.smart_pwr_down", &emx_smart_pwr_down);
TUNABLE_INT("hw.emx.sbp", &emx_debug_sbp);
TUNABLE_INT("hw.emx.82573_workaround", &emx_82573_workaround);

/* Global used in WOL setup with multiport cards */
static int	emx_global_quad_port_a = 0;

/* Set this to one to display debug statistics */
static int	emx_display_debug_stats = 0;

#if !defined(KTR_IF_EMX)
#define KTR_IF_EMX	KTR_ALL
#endif
KTR_INFO_MASTER(if_emx);
KTR_INFO(KTR_IF_EMX, if_emx, intr_beg, 0, "intr begin", 0);
KTR_INFO(KTR_IF_EMX, if_emx, intr_end, 1, "intr end", 0);
KTR_INFO(KTR_IF_EMX, if_emx, pkt_receive, 4, "rx packet", 0);
KTR_INFO(KTR_IF_EMX, if_emx, pkt_txqueue, 5, "tx packet", 0);
KTR_INFO(KTR_IF_EMX, if_emx, pkt_txclean, 6, "tx clean", 0);
#define logif(name)	KTR_LOG(if_emx_ ## name)

static __inline void
emx_setup_rxdesc(emx_rxdesc_t *rxd, const struct emx_rxbuf *rxbuf)
{
	rxd->rxd_bufaddr = htole64(rxbuf->paddr);
	/* DD bit must be cleared */
	rxd->rxd_staterr = 0;
}

static __inline void
emx_rxcsum(uint32_t staterr, struct mbuf *mp)
{
	/* Ignore Checksum bit is set */
	if (staterr & E1000_RXD_STAT_IXSM)
		return;

	if ((staterr & (E1000_RXD_STAT_IPCS | E1000_RXDEXT_STATERR_IPE)) ==
	    E1000_RXD_STAT_IPCS)
		mp->m_pkthdr.csum_flags |= CSUM_IP_CHECKED | CSUM_IP_VALID;

	if ((staterr & (E1000_RXD_STAT_TCPCS | E1000_RXDEXT_STATERR_TCPE)) ==
	    E1000_RXD_STAT_TCPCS) {
		mp->m_pkthdr.csum_flags |= CSUM_DATA_VALID |
					   CSUM_PSEUDO_HDR |
					   CSUM_FRAG_NOT_CHECKED;
		mp->m_pkthdr.csum_data = htons(0xffff);
	}
}

static int
emx_probe(device_t dev)
{
	const struct emx_device *d;
	uint16_t vid, did;

	vid = pci_get_vendor(dev);
	did = pci_get_device(dev);

	for (d = emx_devices; d->desc != NULL; ++d) {
		if (vid == d->vid && did == d->did) {
			device_set_desc(dev, d->desc);
			device_set_async_attach(dev, TRUE);
			return 0;
		}
	}
	return ENXIO;
}

static int
emx_attach(device_t dev)
{
	struct emx_softc *sc = device_get_softc(dev);
	struct ifnet *ifp = &sc->arpcom.ac_if;
	int error = 0;
	uint16_t eeprom_data, device_id;

	callout_init(&sc->timer);

	sc->dev = sc->osdep.dev = dev;

	/*
	 * Determine hardware and mac type
	 */
	sc->hw.vendor_id = pci_get_vendor(dev);
	sc->hw.device_id = pci_get_device(dev);
	sc->hw.revision_id = pci_get_revid(dev);
	sc->hw.subsystem_vendor_id = pci_get_subvendor(dev);
	sc->hw.subsystem_device_id = pci_get_subdevice(dev);

	if (e1000_set_mac_type(&sc->hw))
		return ENXIO;

	/* Enable bus mastering */
	pci_enable_busmaster(dev);

	/*
	 * Allocate IO memory
	 */
	sc->memory_rid = EMX_BAR_MEM;
	sc->memory = bus_alloc_resource_any(dev, SYS_RES_MEMORY,
					    &sc->memory_rid, RF_ACTIVE);
	if (sc->memory == NULL) {
		device_printf(dev, "Unable to allocate bus resource: memory\n");
		error = ENXIO;
		goto fail;
	}
	sc->osdep.mem_bus_space_tag = rman_get_bustag(sc->memory);
	sc->osdep.mem_bus_space_handle = rman_get_bushandle(sc->memory);

	/* XXX This is quite goofy, it is not actually used */
	sc->hw.hw_addr = (uint8_t *)&sc->osdep.mem_bus_space_handle;

	/*
	 * Allocate interrupt
	 */
	sc->intr_rid = 0;
	sc->intr_res = bus_alloc_resource_any(dev, SYS_RES_IRQ, &sc->intr_rid,
					      RF_SHAREABLE | RF_ACTIVE);
	if (sc->intr_res == NULL) {
		device_printf(dev, "Unable to allocate bus resource: "
		    "interrupt\n");
		error = ENXIO;
		goto fail;
	}

	/* Save PCI command register for Shared Code */
	sc->hw.bus.pci_cmd_word = pci_read_config(dev, PCIR_COMMAND, 2);
	sc->hw.back = &sc->osdep;

	/* Do Shared Code initialization */
	if (e1000_setup_init_funcs(&sc->hw, TRUE)) {
		device_printf(dev, "Setup of Shared code failed\n");
		error = ENXIO;
		goto fail;
	}
	e1000_get_bus_info(&sc->hw);

	sc->hw.mac.autoneg = EMX_DO_AUTO_NEG;
	sc->hw.phy.autoneg_wait_to_complete = FALSE;
	sc->hw.phy.autoneg_advertised = EMX_AUTONEG_ADV_DEFAULT;

	/*
	 * Interrupt throttle rate
	 */
	if (emx_int_throttle_ceil == 0) {
		sc->int_throttle_ceil = 0;
	} else {
		int throttle = emx_int_throttle_ceil;

		if (throttle < 0)
			throttle = EMX_DEFAULT_ITR;

		/* Recalculate the tunable value to get the exact frequency. */
		throttle = 1000000000 / 256 / throttle;

		/* Upper 16bits of ITR is reserved and should be zero */
		if (throttle & 0xffff0000)
			throttle = 1000000000 / 256 / EMX_DEFAULT_ITR;

		sc->int_throttle_ceil = 1000000000 / 256 / throttle;
	}

	e1000_init_script_state_82541(&sc->hw, TRUE);
	e1000_set_tbi_compatibility_82543(&sc->hw, TRUE);

	/* Copper options */
	if (sc->hw.phy.media_type == e1000_media_type_copper) {
		sc->hw.phy.mdix = EMX_AUTO_ALL_MODES;
		sc->hw.phy.disable_polarity_correction = FALSE;
		sc->hw.phy.ms_type = EMX_MASTER_SLAVE;
	}

	/* Set the frame limits assuming standard ethernet sized frames. */
	sc->max_frame_size = ETHERMTU + ETHER_HDR_LEN + ETHER_CRC_LEN;
	sc->min_frame_size = ETHER_MIN_LEN;

	/* This controls when hardware reports transmit completion status. */
	sc->hw.mac.report_tx_early = 1;

#ifdef RSS
	/* Calculate # of RX rings */
	if (ncpus > 1)
		sc->rx_ring_cnt = EMX_NRX_RING;
	else
#endif
		sc->rx_ring_cnt = 1;
	sc->rx_ring_inuse = sc->rx_ring_cnt;

	/* Allocate RX/TX rings' busdma(9) stuffs */
	error = emx_dma_alloc(sc);
	if (error)
		goto fail;

	/* Make sure we have a good EEPROM before we read from it */
	if (e1000_validate_nvm_checksum(&sc->hw) < 0) {
		/*
		 * Some PCI-E parts fail the first check due to
		 * the link being in sleep state, call it again,
		 * if it fails a second time its a real issue.
		 */
		if (e1000_validate_nvm_checksum(&sc->hw) < 0) {
			device_printf(dev,
			    "The EEPROM Checksum Is Not Valid\n");
			error = EIO;
			goto fail;
		}
	}

	/* Initialize the hardware */
	error = emx_hw_init(sc);
	if (error) {
		device_printf(dev, "Unable to initialize the hardware\n");
		goto fail;
	}

	/* Copy the permanent MAC address out of the EEPROM */
	if (e1000_read_mac_addr(&sc->hw) < 0) {
		device_printf(dev, "EEPROM read error while reading MAC"
		    " address\n");
		error = EIO;
		goto fail;
	}
	if (!emx_is_valid_eaddr(sc->hw.mac.addr)) {
		device_printf(dev, "Invalid MAC address\n");
		error = EIO;
		goto fail;
	}

	/* Manually turn off all interrupts */
	E1000_WRITE_REG(&sc->hw, E1000_IMC, 0xffffffff);

	/* Setup OS specific network interface */
	emx_setup_ifp(sc);

	/* Add sysctl tree, must after emx_setup_ifp() */
	emx_add_sysctl(sc);

	/* Initialize statistics */
	emx_update_stats(sc);

	sc->hw.mac.get_link_status = 1;
	emx_update_link_status(sc);

	/* Indicate SOL/IDER usage */
	if (e1000_check_reset_block(&sc->hw)) {
		device_printf(dev,
		    "PHY reset is blocked due to SOL/IDER session.\n");
	}

	/* Determine if we have to control management hardware */
	sc->has_manage = e1000_enable_mng_pass_thru(&sc->hw);

	/*
	 * Setup Wake-on-Lan
	 */
	switch (sc->hw.mac.type) {
	case e1000_82571:
	case e1000_80003es2lan:
		if (sc->hw.bus.func == 1) {
			e1000_read_nvm(&sc->hw,
			    NVM_INIT_CONTROL3_PORT_B, 1, &eeprom_data);
		} else {
			e1000_read_nvm(&sc->hw,
			    NVM_INIT_CONTROL3_PORT_A, 1, &eeprom_data);
		}
		eeprom_data &= EMX_EEPROM_APME;
		break;

	default:
		/* APME bit in EEPROM is mapped to WUC.APME */
		eeprom_data =
		    E1000_READ_REG(&sc->hw, E1000_WUC) & E1000_WUC_APME;
		break;
	}
	if (eeprom_data)
		sc->wol = E1000_WUFC_MAG;
	/*
         * We have the eeprom settings, now apply the special cases
         * where the eeprom may be wrong or the board won't support
         * wake on lan on a particular port
	 */
	device_id = pci_get_device(dev);
        switch (device_id) {
	case E1000_DEV_ID_82571EB_FIBER:
		/*
		 * Wake events only supported on port A for dual fiber
		 * regardless of eeprom setting
		 */
		if (E1000_READ_REG(&sc->hw, E1000_STATUS) &
		    E1000_STATUS_FUNC_1)
			sc->wol = 0;
		break;

	case E1000_DEV_ID_82571EB_QUAD_COPPER:
	case E1000_DEV_ID_82571EB_QUAD_FIBER:
	case E1000_DEV_ID_82571EB_QUAD_COPPER_LP:
                /* if quad port sc, disable WoL on all but port A */
		if (emx_global_quad_port_a != 0)
			sc->wol = 0;
		/* Reset for multiple quad port adapters */
		if (++emx_global_quad_port_a == 4)
			emx_global_quad_port_a = 0;
                break;
	}

	/* XXX disable wol */
	sc->wol = 0;

	sc->spare_tx_desc = EMX_TX_SPARE;

	/*
	 * Keep following relationship between spare_tx_desc, oact_tx_desc
	 * and tx_int_nsegs:
	 * (spare_tx_desc + EMX_TX_RESERVED) <=
	 * oact_tx_desc <= EMX_TX_OACTIVE_MAX <= tx_int_nsegs
	 */
	sc->oact_tx_desc = sc->num_tx_desc / 8;
	if (sc->oact_tx_desc > EMX_TX_OACTIVE_MAX)
		sc->oact_tx_desc = EMX_TX_OACTIVE_MAX;
	if (sc->oact_tx_desc < sc->spare_tx_desc + EMX_TX_RESERVED)
		sc->oact_tx_desc = sc->spare_tx_desc + EMX_TX_RESERVED;

	sc->tx_int_nsegs = sc->num_tx_desc / 16;
	if (sc->tx_int_nsegs < sc->oact_tx_desc)
		sc->tx_int_nsegs = sc->oact_tx_desc;

	error = bus_setup_intr(dev, sc->intr_res, INTR_MPSAFE, emx_intr, sc,
			       &sc->intr_tag, ifp->if_serializer);
	if (error) {
		device_printf(dev, "Failed to register interrupt handler");
		ether_ifdetach(&sc->arpcom.ac_if);
		goto fail;
	}

	ifp->if_cpuid = ithread_cpuid(rman_get_start(sc->intr_res));
	KKASSERT(ifp->if_cpuid >= 0 && ifp->if_cpuid < ncpus);
	return (0);
fail:
	emx_detach(dev);
	return (error);
}

static int
emx_detach(device_t dev)
{
	struct emx_softc *sc = device_get_softc(dev);

	if (device_is_attached(dev)) {
		struct ifnet *ifp = &sc->arpcom.ac_if;

		lwkt_serialize_enter(ifp->if_serializer);

		emx_stop(sc);

		e1000_phy_hw_reset(&sc->hw);

		emx_rel_mgmt(sc);

		if (sc->hw.mac.type == e1000_82573 &&
		    e1000_check_mng_mode(&sc->hw))
			emx_rel_hw_control(sc);

		if (sc->wol) {
			E1000_WRITE_REG(&sc->hw, E1000_WUC, E1000_WUC_PME_EN);
			E1000_WRITE_REG(&sc->hw, E1000_WUFC, sc->wol);
			emx_enable_wol(dev);
		}

		bus_teardown_intr(dev, sc->intr_res, sc->intr_tag);

		lwkt_serialize_exit(ifp->if_serializer);

		ether_ifdetach(ifp);
	}
	bus_generic_detach(dev);

	if (sc->intr_res != NULL) {
		bus_release_resource(dev, SYS_RES_IRQ, sc->intr_rid,
				     sc->intr_res);
	}

	if (sc->memory != NULL) {
		bus_release_resource(dev, SYS_RES_MEMORY, sc->memory_rid,
				     sc->memory);
	}

	emx_dma_free(sc);

	/* Free sysctl tree */
	if (sc->sysctl_tree != NULL)
		sysctl_ctx_free(&sc->sysctl_ctx);

	return (0);
}

static int
emx_shutdown(device_t dev)
{
	return emx_suspend(dev);
}

static int
emx_suspend(device_t dev)
{
	struct emx_softc *sc = device_get_softc(dev);
	struct ifnet *ifp = &sc->arpcom.ac_if;

	lwkt_serialize_enter(ifp->if_serializer);

	emx_stop(sc);

	emx_rel_mgmt(sc);

        if (sc->hw.mac.type == e1000_82573 &&
            e1000_check_mng_mode(&sc->hw))
                emx_rel_hw_control(sc);

        if (sc->wol) {
		E1000_WRITE_REG(&sc->hw, E1000_WUC, E1000_WUC_PME_EN);
		E1000_WRITE_REG(&sc->hw, E1000_WUFC, sc->wol);
		emx_enable_wol(dev);
        }

	lwkt_serialize_exit(ifp->if_serializer);

	return bus_generic_suspend(dev);
}

static int
emx_resume(device_t dev)
{
	struct emx_softc *sc = device_get_softc(dev);
	struct ifnet *ifp = &sc->arpcom.ac_if;

	lwkt_serialize_enter(ifp->if_serializer);

	emx_init(sc);
	emx_get_mgmt(sc);
	if_devstart(ifp);

	lwkt_serialize_exit(ifp->if_serializer);

	return bus_generic_resume(dev);
}

static void
emx_start(struct ifnet *ifp)
{
	struct emx_softc *sc = ifp->if_softc;
	struct mbuf *m_head;

	ASSERT_SERIALIZED(ifp->if_serializer);

	if ((ifp->if_flags & (IFF_RUNNING | IFF_OACTIVE)) != IFF_RUNNING)
		return;

	if (!sc->link_active) {
		ifq_purge(&ifp->if_snd);
		return;
	}

	while (!ifq_is_empty(&ifp->if_snd)) {
		/* Now do we at least have a minimal? */
		if (EMX_IS_OACTIVE(sc)) {
			emx_tx_collect(sc);
			if (EMX_IS_OACTIVE(sc)) {
				ifp->if_flags |= IFF_OACTIVE;
				sc->no_tx_desc_avail1++;
				break;
			}
		}

		logif(pkt_txqueue);
		m_head = ifq_dequeue(&ifp->if_snd, NULL);
		if (m_head == NULL)
			break;

		if (emx_encap(sc, &m_head)) {
			ifp->if_oerrors++;
			emx_tx_collect(sc);
			continue;
		}

		/* Send a copy of the frame to the BPF listener */
		ETHER_BPF_MTAP(ifp, m_head);

		/* Set timeout in case hardware has problems transmitting. */
		ifp->if_timer = EMX_TX_TIMEOUT;
	}
}

static int
emx_ioctl(struct ifnet *ifp, u_long command, caddr_t data, struct ucred *cr)
{
	struct emx_softc *sc = ifp->if_softc;
	struct ifreq *ifr = (struct ifreq *)data;
	uint16_t eeprom_data = 0;
	int max_frame_size, mask, reinit;
	int error = 0;

	ASSERT_SERIALIZED(ifp->if_serializer);

	switch (command) {
	case SIOCSIFMTU:
		switch (sc->hw.mac.type) {
		case e1000_82573:
			/*
			 * 82573 only supports jumbo frames
			 * if ASPM is disabled.
			 */
			e1000_read_nvm(&sc->hw, NVM_INIT_3GIO_3, 1,
				       &eeprom_data);
			if (eeprom_data & NVM_WORD1A_ASPM_MASK) {
				max_frame_size = ETHER_MAX_LEN;
				break;
			}
			/* FALL THROUGH */

		/* Limit Jumbo Frame size */
		case e1000_82571:
		case e1000_82572:
		case e1000_82574:
		case e1000_80003es2lan:
			max_frame_size = 9234;
			break;

		default:
			max_frame_size = MAX_JUMBO_FRAME_SIZE;
			break;
		}
		if (ifr->ifr_mtu > max_frame_size - ETHER_HDR_LEN -
		    ETHER_CRC_LEN) {
			error = EINVAL;
			break;
		}

		ifp->if_mtu = ifr->ifr_mtu;
		sc->max_frame_size = ifp->if_mtu + ETHER_HDR_LEN +
				     ETHER_CRC_LEN;

		if (ifp->if_flags & IFF_RUNNING)
			emx_init(sc);
		break;

	case SIOCSIFFLAGS:
		if (ifp->if_flags & IFF_UP) {
			if ((ifp->if_flags & IFF_RUNNING)) {
				if ((ifp->if_flags ^ sc->if_flags) &
				    (IFF_PROMISC | IFF_ALLMULTI)) {
					emx_disable_promisc(sc);
					emx_set_promisc(sc);
				}
			} else {
				emx_init(sc);
			}
		} else if (ifp->if_flags & IFF_RUNNING) {
			emx_stop(sc);
		}
		sc->if_flags = ifp->if_flags;
		break;

	case SIOCADDMULTI:
	case SIOCDELMULTI:
		if (ifp->if_flags & IFF_RUNNING) {
			emx_disable_intr(sc);
			emx_set_multi(sc);
#ifdef DEVICE_POLLING
			if (!(ifp->if_flags & IFF_POLLING))
#endif
				emx_enable_intr(sc);
		}
		break;

	case SIOCSIFMEDIA:
		/* Check SOL/IDER usage */
		if (e1000_check_reset_block(&sc->hw)) {
			device_printf(sc->dev, "Media change is"
			    " blocked due to SOL/IDER session.\n");
			break;
		}
		/* FALL THROUGH */

	case SIOCGIFMEDIA:
		error = ifmedia_ioctl(ifp, ifr, &sc->media, command);
		break;

	case SIOCSIFCAP:
		reinit = 0;
		mask = ifr->ifr_reqcap ^ ifp->if_capenable;
		if (mask & IFCAP_HWCSUM) {
			ifp->if_capenable ^= (mask & IFCAP_HWCSUM);
			reinit = 1;
		}
		if (mask & IFCAP_VLAN_HWTAGGING) {
			ifp->if_capenable ^= IFCAP_VLAN_HWTAGGING;
			reinit = 1;
		}
		if (mask & IFCAP_RSS) {
			ifp->if_capenable ^= IFCAP_RSS;
			reinit = 1;
		}
		if (reinit && (ifp->if_flags & IFF_RUNNING))
			emx_init(sc);
		break;

	default:
		error = ether_ioctl(ifp, command, data);
		break;
	}
	return (error);
}

static void
emx_watchdog(struct ifnet *ifp)
{
	struct emx_softc *sc = ifp->if_softc;

	ASSERT_SERIALIZED(ifp->if_serializer);

	/*
	 * The timer is set to 5 every time start queues a packet.
	 * Then txeof keeps resetting it as long as it cleans at
	 * least one descriptor.
	 * Finally, anytime all descriptors are clean the timer is
	 * set to 0.
	 */

	if (E1000_READ_REG(&sc->hw, E1000_TDT(0)) ==
	    E1000_READ_REG(&sc->hw, E1000_TDH(0))) {
		/*
		 * If we reach here, all TX jobs are completed and
		 * the TX engine should have been idled for some time.
		 * We don't need to call if_devstart() here.
		 */
		ifp->if_flags &= ~IFF_OACTIVE;
		ifp->if_timer = 0;
		return;
	}

	/*
	 * If we are in this routine because of pause frames, then
	 * don't reset the hardware.
	 */
	if (E1000_READ_REG(&sc->hw, E1000_STATUS) & E1000_STATUS_TXOFF) {
		ifp->if_timer = EMX_TX_TIMEOUT;
		return;
	}

	if (e1000_check_for_link(&sc->hw) == 0)
		if_printf(ifp, "watchdog timeout -- resetting\n");

	ifp->if_oerrors++;
	sc->watchdog_events++;

	emx_init(sc);

	if (!ifq_is_empty(&ifp->if_snd))
		if_devstart(ifp);
}

static void
emx_init(void *xsc)
{
	struct emx_softc *sc = xsc;
	struct ifnet *ifp = &sc->arpcom.ac_if;
	device_t dev = sc->dev;
	uint32_t pba;
	int i;

	ASSERT_SERIALIZED(ifp->if_serializer);

	emx_stop(sc);

	/*
	 * Packet Buffer Allocation (PBA)
	 * Writing PBA sets the receive portion of the buffer
	 * the remainder is used for the transmit buffer.
	 */
	switch (sc->hw.mac.type) {
	/* Total Packet Buffer on these is 48K */
	case e1000_82571:
	case e1000_82572:
	case e1000_80003es2lan:
		pba = E1000_PBA_32K; /* 32K for Rx, 16K for Tx */
		break;

	case e1000_82573: /* 82573: Total Packet Buffer is 32K */
		pba = E1000_PBA_12K; /* 12K for Rx, 20K for Tx */
		break;

	case e1000_82574:
		pba = E1000_PBA_20K; /* 20K for Rx, 20K for Tx */
		break;

	default:
		/* Devices before 82547 had a Packet Buffer of 64K.   */
		if (sc->max_frame_size > 8192)
			pba = E1000_PBA_40K; /* 40K for Rx, 24K for Tx */
		else
			pba = E1000_PBA_48K; /* 48K for Rx, 16K for Tx */
	}
	E1000_WRITE_REG(&sc->hw, E1000_PBA, pba);

	/* Get the latest mac address, User can use a LAA */
        bcopy(IF_LLADDR(ifp), sc->hw.mac.addr, ETHER_ADDR_LEN);

	/* Put the address into the Receive Address Array */
	e1000_rar_set(&sc->hw, sc->hw.mac.addr, 0);

	/*
	 * With the 82571 sc, RAR[0] may be overwritten
	 * when the other port is reset, we make a duplicate
	 * in RAR[14] for that eventuality, this assures
	 * the interface continues to function.
	 */
	if (sc->hw.mac.type == e1000_82571) {
		e1000_set_laa_state_82571(&sc->hw, TRUE);
		e1000_rar_set(&sc->hw, sc->hw.mac.addr,
		    E1000_RAR_ENTRIES - 1);
	}

	/* Initialize the hardware */
	if (emx_hw_init(sc)) {
		device_printf(dev, "Unable to initialize the hardware\n");
		/* XXX emx_stop()? */
		return;
	}
	emx_update_link_status(sc);

	/* Setup VLAN support, basic and offload if available */
	E1000_WRITE_REG(&sc->hw, E1000_VET, ETHERTYPE_VLAN);

	if (ifp->if_capenable & IFCAP_VLAN_HWTAGGING) {
		uint32_t ctrl;

		ctrl = E1000_READ_REG(&sc->hw, E1000_CTRL);
		ctrl |= E1000_CTRL_VME;
		E1000_WRITE_REG(&sc->hw, E1000_CTRL, ctrl);
	}

	/* Set hardware offload abilities */
	if (ifp->if_capenable & IFCAP_TXCSUM)
		ifp->if_hwassist = EMX_CSUM_FEATURES;
	else
		ifp->if_hwassist = 0;

	/* Configure for OS presence */
	emx_get_mgmt(sc);

	/* Prepare transmit descriptors and buffers */
	emx_init_tx_ring(sc);
	emx_init_tx_unit(sc);

	/* Setup Multicast table */
	emx_set_multi(sc);

	/*
	 * Adjust # of RX ring to be used based on IFCAP_RSS
	 */
	if (ifp->if_capenable & IFCAP_RSS)
		sc->rx_ring_inuse = sc->rx_ring_cnt;
	else
		sc->rx_ring_inuse = 1;

	/* Prepare receive descriptors and buffers */
	for (i = 0; i < sc->rx_ring_inuse; ++i) {
		if (emx_init_rx_ring(sc, &sc->rx_data[i])) {
			device_printf(dev,
			    "Could not setup receive structures\n");
			emx_stop(sc);
			return;
		}
	}
	emx_init_rx_unit(sc);

	/* Don't lose promiscuous settings */
	emx_set_promisc(sc);

	ifp->if_flags |= IFF_RUNNING;
	ifp->if_flags &= ~IFF_OACTIVE;

	callout_reset(&sc->timer, hz, emx_timer, sc);
	e1000_clear_hw_cntrs_base_generic(&sc->hw);

	/* MSI/X configuration for 82574 */
	if (sc->hw.mac.type == e1000_82574) {
		int tmp;

		tmp = E1000_READ_REG(&sc->hw, E1000_CTRL_EXT);
		tmp |= E1000_CTRL_EXT_PBA_CLR;
		E1000_WRITE_REG(&sc->hw, E1000_CTRL_EXT, tmp);
		/*
		 * Set the IVAR - interrupt vector routing.
		 * Each nibble represents a vector, high bit
		 * is enable, other 3 bits are the MSIX table
		 * entry, we map RXQ0 to 0, TXQ0 to 1, and
		 * Link (other) to 2, hence the magic number.
		 */
		E1000_WRITE_REG(&sc->hw, E1000_IVAR, 0x800A0908);
	}

#ifdef DEVICE_POLLING
	/*
	 * Only enable interrupts if we are not polling, make sure
	 * they are off otherwise.
	 */
	if (ifp->if_flags & IFF_POLLING)
		emx_disable_intr(sc);
	else
#endif /* DEVICE_POLLING */
		emx_enable_intr(sc);

	/* Don't reset the phy next time init gets called */
	sc->hw.phy.reset_disable = TRUE;
}

#ifdef DEVICE_POLLING

static void
emx_poll(struct ifnet *ifp, enum poll_cmd cmd, int count)
{
	struct emx_softc *sc = ifp->if_softc;
	uint32_t reg_icr;

	ASSERT_SERIALIZED(ifp->if_serializer);

	switch (cmd) {
	case POLL_REGISTER:
		emx_disable_intr(sc);
		break;

	case POLL_DEREGISTER:
		emx_enable_intr(sc);
		break;

	case POLL_AND_CHECK_STATUS:
		reg_icr = E1000_READ_REG(&sc->hw, E1000_ICR);
		if (reg_icr & (E1000_ICR_RXSEQ | E1000_ICR_LSC)) {
			callout_stop(&sc->timer);
			sc->hw.mac.get_link_status = 1;
			emx_update_link_status(sc);
			callout_reset(&sc->timer, hz, emx_timer, sc);
		}
		/* FALL THROUGH */
	case POLL_ONLY:
		if (ifp->if_flags & IFF_RUNNING) {
			int i;

			for (i = 0; i < sc->rx_ring_inuse; ++i)
				emx_rxeof(sc, i, count);

			emx_txeof(sc);
			if (!ifq_is_empty(&ifp->if_snd))
				if_devstart(ifp);
		}
		break;
	}
}

#endif /* DEVICE_POLLING */

static void
emx_intr(void *xsc)
{
	struct emx_softc *sc = xsc;
	struct ifnet *ifp = &sc->arpcom.ac_if;
	uint32_t reg_icr;

	logif(intr_beg);
	ASSERT_SERIALIZED(ifp->if_serializer);

	reg_icr = E1000_READ_REG(&sc->hw, E1000_ICR);

	if ((reg_icr & E1000_ICR_INT_ASSERTED) == 0) {
		logif(intr_end);
		return;
	}

	/*
	 * XXX: some laptops trigger several spurious interrupts
	 * on em(4) when in the resume cycle. The ICR register
	 * reports all-ones value in this case. Processing such
	 * interrupts would lead to a freeze. I don't know why.
	 */
	if (reg_icr == 0xffffffff) {
		logif(intr_end);
		return;
	}

	if (ifp->if_flags & IFF_RUNNING) {
		if (reg_icr &
		    (E1000_ICR_RXT0 | E1000_ICR_RXDMT0 | E1000_ICR_RXO)) {
			int i;

			for (i = 0; i < sc->rx_ring_inuse; ++i)
				emx_rxeof(sc, i, -1);
		}
		if (reg_icr & E1000_ICR_TXDW) {
			emx_txeof(sc);
			if (!ifq_is_empty(&ifp->if_snd))
				if_devstart(ifp);
		}
	}

	/* Link status change */
	if (reg_icr & (E1000_ICR_RXSEQ | E1000_ICR_LSC)) {
		callout_stop(&sc->timer);
		sc->hw.mac.get_link_status = 1;
		emx_update_link_status(sc);

		/* Deal with TX cruft when link lost */
		emx_tx_purge(sc);

		callout_reset(&sc->timer, hz, emx_timer, sc);
	}

	if (reg_icr & E1000_ICR_RXO)
		sc->rx_overruns++;

	logif(intr_end);
}

static void
emx_media_status(struct ifnet *ifp, struct ifmediareq *ifmr)
{
	struct emx_softc *sc = ifp->if_softc;

	ASSERT_SERIALIZED(ifp->if_serializer);

	emx_update_link_status(sc);

	ifmr->ifm_status = IFM_AVALID;
	ifmr->ifm_active = IFM_ETHER;

	if (!sc->link_active)
		return;

	ifmr->ifm_status |= IFM_ACTIVE;

	if (sc->hw.phy.media_type == e1000_media_type_fiber ||
	    sc->hw.phy.media_type == e1000_media_type_internal_serdes) {
		ifmr->ifm_active |= IFM_1000_SX | IFM_FDX;
	} else {
		switch (sc->link_speed) {
		case 10:
			ifmr->ifm_active |= IFM_10_T;
			break;
		case 100:
			ifmr->ifm_active |= IFM_100_TX;
			break;

		case 1000:
			ifmr->ifm_active |= IFM_1000_T;
			break;
		}
		if (sc->link_duplex == FULL_DUPLEX)
			ifmr->ifm_active |= IFM_FDX;
		else
			ifmr->ifm_active |= IFM_HDX;
	}
}

static int
emx_media_change(struct ifnet *ifp)
{
	struct emx_softc *sc = ifp->if_softc;
	struct ifmedia *ifm = &sc->media;

	ASSERT_SERIALIZED(ifp->if_serializer);

	if (IFM_TYPE(ifm->ifm_media) != IFM_ETHER)
		return (EINVAL);

	switch (IFM_SUBTYPE(ifm->ifm_media)) {
	case IFM_AUTO:
		sc->hw.mac.autoneg = EMX_DO_AUTO_NEG;
		sc->hw.phy.autoneg_advertised = EMX_AUTONEG_ADV_DEFAULT;
		break;

	case IFM_1000_LX:
	case IFM_1000_SX:
	case IFM_1000_T:
		sc->hw.mac.autoneg = EMX_DO_AUTO_NEG;
		sc->hw.phy.autoneg_advertised = ADVERTISE_1000_FULL;
		break;

	case IFM_100_TX:
		sc->hw.mac.autoneg = FALSE;
		sc->hw.phy.autoneg_advertised = 0;
		if ((ifm->ifm_media & IFM_GMASK) == IFM_FDX)
			sc->hw.mac.forced_speed_duplex = ADVERTISE_100_FULL;
		else
			sc->hw.mac.forced_speed_duplex = ADVERTISE_100_HALF;
		break;

	case IFM_10_T:
		sc->hw.mac.autoneg = FALSE;
		sc->hw.phy.autoneg_advertised = 0;
		if ((ifm->ifm_media & IFM_GMASK) == IFM_FDX)
			sc->hw.mac.forced_speed_duplex = ADVERTISE_10_FULL;
		else
			sc->hw.mac.forced_speed_duplex = ADVERTISE_10_HALF;
		break;

	default:
		if_printf(ifp, "Unsupported media type\n");
		break;
	}

	/*
	 * As the speed/duplex settings my have changed we need to
	 * reset the PHY.
	 */
	sc->hw.phy.reset_disable = FALSE;

	emx_init(sc);

	return (0);
}

static int
emx_encap(struct emx_softc *sc, struct mbuf **m_headp)
{
	bus_dma_segment_t segs[EMX_MAX_SCATTER];
	bus_dmamap_t map;
	struct emx_txbuf *tx_buffer, *tx_buffer_mapped;
	struct e1000_tx_desc *ctxd = NULL;
	struct mbuf *m_head = *m_headp;
	uint32_t txd_upper, txd_lower, cmd = 0;
	int maxsegs, nsegs, i, j, first, last = 0, error;

	if (m_head->m_len < EMX_TXCSUM_MINHL &&
	    (m_head->m_flags & EMX_CSUM_FEATURES)) {
		/*
		 * Make sure that ethernet header and ip.ip_hl are in
		 * contiguous memory, since if TXCSUM is enabled, later
		 * TX context descriptor's setup need to access ip.ip_hl.
		 */
		error = emx_txcsum_pullup(sc, m_headp);
		if (error) {
			KKASSERT(*m_headp == NULL);
			return error;
		}
		m_head = *m_headp;
	}

	txd_upper = txd_lower = 0;

	/*
	 * Capture the first descriptor index, this descriptor
	 * will have the index of the EOP which is the only one
	 * that now gets a DONE bit writeback.
	 */
	first = sc->next_avail_tx_desc;
	tx_buffer = &sc->tx_buf[first];
	tx_buffer_mapped = tx_buffer;
	map = tx_buffer->map;

	maxsegs = sc->num_tx_desc_avail - EMX_TX_RESERVED;
	KASSERT(maxsegs >= sc->spare_tx_desc, ("not enough spare TX desc\n"));
	if (maxsegs > EMX_MAX_SCATTER)
		maxsegs = EMX_MAX_SCATTER;

	error = bus_dmamap_load_mbuf_defrag(sc->txtag, map, m_headp,
			segs, maxsegs, &nsegs, BUS_DMA_NOWAIT);
	if (error) {
		if (error == ENOBUFS)
			sc->mbuf_alloc_failed++;
		else
			sc->no_tx_dma_setup++;

		m_freem(*m_headp);
		*m_headp = NULL;
		return error;
	}
        bus_dmamap_sync(sc->txtag, map, BUS_DMASYNC_PREWRITE);

	m_head = *m_headp;
	sc->tx_nsegs += nsegs;

	if (m_head->m_pkthdr.csum_flags & EMX_CSUM_FEATURES) {
		/* TX csum offloading will consume one TX desc */
		sc->tx_nsegs += emx_txcsum(sc, m_head, &txd_upper, &txd_lower);
	}
	i = sc->next_avail_tx_desc;

	/* Set up our transmit descriptors */
	for (j = 0; j < nsegs; j++) {
		tx_buffer = &sc->tx_buf[i];
		ctxd = &sc->tx_desc_base[i];

		ctxd->buffer_addr = htole64(segs[j].ds_addr);
		ctxd->lower.data = htole32(E1000_TXD_CMD_IFCS |
					   txd_lower | segs[j].ds_len);
		ctxd->upper.data = htole32(txd_upper);

		last = i;
		if (++i == sc->num_tx_desc)
			i = 0;
	}

	sc->next_avail_tx_desc = i;

	KKASSERT(sc->num_tx_desc_avail > nsegs);
	sc->num_tx_desc_avail -= nsegs;

        /* Handle VLAN tag */
	if (m_head->m_flags & M_VLANTAG) {
		/* Set the vlan id. */
		ctxd->upper.fields.special =
		    htole16(m_head->m_pkthdr.ether_vlantag);

		/* Tell hardware to add tag */
		ctxd->lower.data |= htole32(E1000_TXD_CMD_VLE);
	}

	tx_buffer->m_head = m_head;
	tx_buffer_mapped->map = tx_buffer->map;
	tx_buffer->map = map;

	if (sc->tx_nsegs >= sc->tx_int_nsegs) {
		sc->tx_nsegs = 0;

		/*
		 * Report Status (RS) is turned on
		 * every tx_int_nsegs descriptors.
		 */
		cmd = E1000_TXD_CMD_RS;

		/*
		 * Keep track of the descriptor, which will
		 * be written back by hardware.
		 */
		sc->tx_dd[sc->tx_dd_tail] = last;
		EMX_INC_TXDD_IDX(sc->tx_dd_tail);
		KKASSERT(sc->tx_dd_tail != sc->tx_dd_head);
	}

	/*
	 * Last Descriptor of Packet needs End Of Packet (EOP)
	 */
	ctxd->lower.data |= htole32(E1000_TXD_CMD_EOP | cmd);

	/*
	 * Advance the Transmit Descriptor Tail (TDT), this tells
	 * the E1000 that this frame is available to transmit.
	 */
	E1000_WRITE_REG(&sc->hw, E1000_TDT(0), i);

	return (0);
}

static void
emx_set_promisc(struct emx_softc *sc)
{
	struct ifnet *ifp = &sc->arpcom.ac_if;
	uint32_t reg_rctl;

	reg_rctl = E1000_READ_REG(&sc->hw, E1000_RCTL);

	if (ifp->if_flags & IFF_PROMISC) {
		reg_rctl |= (E1000_RCTL_UPE | E1000_RCTL_MPE);
		/* Turn this on if you want to see bad packets */
		if (emx_debug_sbp)
			reg_rctl |= E1000_RCTL_SBP;
		E1000_WRITE_REG(&sc->hw, E1000_RCTL, reg_rctl);
	} else if (ifp->if_flags & IFF_ALLMULTI) {
		reg_rctl |= E1000_RCTL_MPE;
		reg_rctl &= ~E1000_RCTL_UPE;
		E1000_WRITE_REG(&sc->hw, E1000_RCTL, reg_rctl);
	}
}

static void
emx_disable_promisc(struct emx_softc *sc)
{
	uint32_t reg_rctl;

	reg_rctl = E1000_READ_REG(&sc->hw, E1000_RCTL);

	reg_rctl &= ~E1000_RCTL_UPE;
	reg_rctl &= ~E1000_RCTL_MPE;
	reg_rctl &= ~E1000_RCTL_SBP;
	E1000_WRITE_REG(&sc->hw, E1000_RCTL, reg_rctl);
}

static void
emx_set_multi(struct emx_softc *sc)
{
	struct ifnet *ifp = &sc->arpcom.ac_if;
	struct ifmultiaddr *ifma;
	uint32_t reg_rctl = 0;
	uint8_t  mta[512]; /* Largest MTS is 4096 bits */
	int mcnt = 0;

	LIST_FOREACH(ifma, &ifp->if_multiaddrs, ifma_link) {
		if (ifma->ifma_addr->sa_family != AF_LINK)
			continue;

		if (mcnt == EMX_MCAST_ADDR_MAX)
			break;

		bcopy(LLADDR((struct sockaddr_dl *)ifma->ifma_addr),
		      &mta[mcnt * ETHER_ADDR_LEN], ETHER_ADDR_LEN);
		mcnt++;
	}

	if (mcnt >= EMX_MCAST_ADDR_MAX) {
		reg_rctl = E1000_READ_REG(&sc->hw, E1000_RCTL);
		reg_rctl |= E1000_RCTL_MPE;
		E1000_WRITE_REG(&sc->hw, E1000_RCTL, reg_rctl);
	} else {
		e1000_update_mc_addr_list(&sc->hw, mta,
		    mcnt, 1, sc->hw.mac.rar_entry_count);
	}
}

/*
 * This routine checks for link status and updates statistics.
 */
static void
emx_timer(void *xsc)
{
	struct emx_softc *sc = xsc;
	struct ifnet *ifp = &sc->arpcom.ac_if;

	lwkt_serialize_enter(ifp->if_serializer);

	emx_update_link_status(sc);
	emx_update_stats(sc);

	/* Reset LAA into RAR[0] on 82571 */
	if (e1000_get_laa_state_82571(&sc->hw) == TRUE)
		e1000_rar_set(&sc->hw, sc->hw.mac.addr, 0);

	if (emx_display_debug_stats && (ifp->if_flags & IFF_RUNNING))
		emx_print_hw_stats(sc);

	emx_smartspeed(sc);

	callout_reset(&sc->timer, hz, emx_timer, sc);

	lwkt_serialize_exit(ifp->if_serializer);
}

static void
emx_update_link_status(struct emx_softc *sc)
{
	struct e1000_hw *hw = &sc->hw;
	struct ifnet *ifp = &sc->arpcom.ac_if;
	device_t dev = sc->dev;
	uint32_t link_check = 0;

	/* Get the cached link value or read phy for real */
	switch (hw->phy.media_type) {
	case e1000_media_type_copper:
		if (hw->mac.get_link_status) {
			/* Do the work to read phy */
			e1000_check_for_link(hw);
			link_check = !hw->mac.get_link_status;
			if (link_check) /* ESB2 fix */
				e1000_cfg_on_link_up(hw);
		} else {
			link_check = TRUE;
		}
		break;

	case e1000_media_type_fiber:
		e1000_check_for_link(hw);
		link_check = E1000_READ_REG(hw, E1000_STATUS) & E1000_STATUS_LU;
		break;

	case e1000_media_type_internal_serdes:
		e1000_check_for_link(hw);
		link_check = sc->hw.mac.serdes_has_link;
		break;

	case e1000_media_type_unknown:
	default:
		break;
	}

	/* Now check for a transition */
	if (link_check && sc->link_active == 0) {
		e1000_get_speed_and_duplex(hw, &sc->link_speed,
		    &sc->link_duplex);

		/*
		 * Check if we should enable/disable SPEED_MODE bit on
		 * 82571EB/82572EI
		 */
		if (hw->mac.type == e1000_82571 ||
		    hw->mac.type == e1000_82572) {
			int tarc0;

			tarc0 = E1000_READ_REG(hw, E1000_TARC(0));
			if (sc->link_speed != SPEED_1000)
				tarc0 &= ~EMX_TARC_SPEED_MODE;
			else
				tarc0 |= EMX_TARC_SPEED_MODE;
			E1000_WRITE_REG(hw, E1000_TARC(0), tarc0);
		}
		if (bootverbose) {
			device_printf(dev, "Link is up %d Mbps %s\n",
			    sc->link_speed,
			    ((sc->link_duplex == FULL_DUPLEX) ?
			    "Full Duplex" : "Half Duplex"));
		}
		sc->link_active = 1;
		sc->smartspeed = 0;
		ifp->if_baudrate = sc->link_speed * 1000000;
		ifp->if_link_state = LINK_STATE_UP;
		if_link_state_change(ifp);
	} else if (!link_check && sc->link_active == 1) {
		ifp->if_baudrate = sc->link_speed = 0;
		sc->link_duplex = 0;
		if (bootverbose)
			device_printf(dev, "Link is Down\n");
		sc->link_active = 0;
#if 0
		/* Link down, disable watchdog */
		if->if_timer = 0;
#endif
		ifp->if_link_state = LINK_STATE_DOWN;
		if_link_state_change(ifp);
	}
}

static void
emx_stop(struct emx_softc *sc)
{
	struct ifnet *ifp = &sc->arpcom.ac_if;
	int i;

	ASSERT_SERIALIZED(ifp->if_serializer);

	emx_disable_intr(sc);

	callout_stop(&sc->timer);

	ifp->if_flags &= ~(IFF_RUNNING | IFF_OACTIVE);
	ifp->if_timer = 0;

	/*
	 * Disable multiple receive queues.
	 *
	 * NOTE:
	 * We should disable multiple receive queues before
	 * resetting the hardware.
	 */
	E1000_WRITE_REG(&sc->hw, E1000_MRQC, 0);

	e1000_reset_hw(&sc->hw);
	E1000_WRITE_REG(&sc->hw, E1000_WUC, 0);

	for (i = 0; i < sc->num_tx_desc; i++) {
		struct emx_txbuf *tx_buffer = &sc->tx_buf[i];

		if (tx_buffer->m_head != NULL) {
			bus_dmamap_unload(sc->txtag, tx_buffer->map);
			m_freem(tx_buffer->m_head);
			tx_buffer->m_head = NULL;
		}
	}

	for (i = 0; i < sc->rx_ring_inuse; ++i)
		emx_free_rx_ring(sc, &sc->rx_data[i]);

	sc->csum_flags = 0;
	sc->csum_ehlen = 0;
	sc->csum_iphlen = 0;

	sc->tx_dd_head = 0;
	sc->tx_dd_tail = 0;
	sc->tx_nsegs = 0;
}

static int
emx_hw_init(struct emx_softc *sc)
{
	device_t dev = sc->dev;
	uint16_t rx_buffer_size;

	/* Issue a global reset */
	e1000_reset_hw(&sc->hw);

	/* Get control from any management/hw control */
	if (sc->hw.mac.type == e1000_82573 &&
	    e1000_check_mng_mode(&sc->hw))
		emx_get_hw_control(sc);

	/* Set up smart power down as default off on newer adapters. */
	if (!emx_smart_pwr_down &&
	    (sc->hw.mac.type == e1000_82571 ||
	     sc->hw.mac.type == e1000_82572)) {
		uint16_t phy_tmp = 0;

		/* Speed up time to link by disabling smart power down. */
		e1000_read_phy_reg(&sc->hw,
		    IGP02E1000_PHY_POWER_MGMT, &phy_tmp);
		phy_tmp &= ~IGP02E1000_PM_SPD;
		e1000_write_phy_reg(&sc->hw,
		    IGP02E1000_PHY_POWER_MGMT, phy_tmp);
	}

	/*
	 * These parameters control the automatic generation (Tx) and
	 * response (Rx) to Ethernet PAUSE frames.
	 * - High water mark should allow for at least two frames to be
	 *   received after sending an XOFF.
	 * - Low water mark works best when it is very near the high water mark.
	 *   This allows the receiver to restart by sending XON when it has
	 *   drained a bit. Here we use an arbitary value of 1500 which will
	 *   restart after one full frame is pulled from the buffer. There
	 *   could be several smaller frames in the buffer and if so they will
	 *   not trigger the XON until their total number reduces the buffer
	 *   by 1500.
	 * - The pause time is fairly large at 1000 x 512ns = 512 usec.
	 */
	rx_buffer_size = (E1000_READ_REG(&sc->hw, E1000_PBA) & 0xffff) << 10;

	sc->hw.fc.high_water = rx_buffer_size -
			       roundup2(sc->max_frame_size, 1024);
	sc->hw.fc.low_water = sc->hw.fc.high_water - 1500;

	if (sc->hw.mac.type == e1000_80003es2lan)
		sc->hw.fc.pause_time = 0xFFFF;
	else
		sc->hw.fc.pause_time = EMX_FC_PAUSE_TIME;
	sc->hw.fc.send_xon = TRUE;
	sc->hw.fc.requested_mode = e1000_fc_full;

	if (e1000_init_hw(&sc->hw) < 0) {
		device_printf(dev, "Hardware Initialization Failed\n");
		return (EIO);
	}

	e1000_check_for_link(&sc->hw);

	return (0);
}

static void
emx_setup_ifp(struct emx_softc *sc)
{
	struct ifnet *ifp = &sc->arpcom.ac_if;

	if_initname(ifp, device_get_name(sc->dev),
		    device_get_unit(sc->dev));
	ifp->if_softc = sc;
	ifp->if_flags = IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST;
	ifp->if_init =  emx_init;
	ifp->if_ioctl = emx_ioctl;
	ifp->if_start = emx_start;
#ifdef DEVICE_POLLING
	ifp->if_poll = emx_poll;
#endif
	ifp->if_watchdog = emx_watchdog;
	ifq_set_maxlen(&ifp->if_snd, sc->num_tx_desc - 1);
	ifq_set_ready(&ifp->if_snd);

	ether_ifattach(ifp, sc->hw.mac.addr, NULL);

	ifp->if_capabilities = IFCAP_HWCSUM |
			       IFCAP_VLAN_HWTAGGING |
			       IFCAP_VLAN_MTU;
	if (sc->rx_ring_cnt > 1)
		ifp->if_capabilities |= IFCAP_RSS;
	ifp->if_capenable = ifp->if_capabilities;
	ifp->if_hwassist = EMX_CSUM_FEATURES;

	/*
	 * Tell the upper layer(s) we support long frames.
	 */
	ifp->if_data.ifi_hdrlen = sizeof(struct ether_vlan_header);

	/*
	 * Specify the media types supported by this sc and register
	 * callbacks to update media and link information
	 */
	ifmedia_init(&sc->media, IFM_IMASK,
		     emx_media_change, emx_media_status);
	if (sc->hw.phy.media_type == e1000_media_type_fiber ||
	    sc->hw.phy.media_type == e1000_media_type_internal_serdes) {
		ifmedia_add(&sc->media, IFM_ETHER | IFM_1000_SX | IFM_FDX,
			    0, NULL);
		ifmedia_add(&sc->media, IFM_ETHER | IFM_1000_SX, 0, NULL);
	} else {
		ifmedia_add(&sc->media, IFM_ETHER | IFM_10_T, 0, NULL);
		ifmedia_add(&sc->media, IFM_ETHER | IFM_10_T | IFM_FDX,
			    0, NULL);
		ifmedia_add(&sc->media, IFM_ETHER | IFM_100_TX, 0, NULL);
		ifmedia_add(&sc->media, IFM_ETHER | IFM_100_TX | IFM_FDX,
			    0, NULL);
		if (sc->hw.phy.type != e1000_phy_ife) {
			ifmedia_add(&sc->media,
				IFM_ETHER | IFM_1000_T | IFM_FDX, 0, NULL);
			ifmedia_add(&sc->media,
				IFM_ETHER | IFM_1000_T, 0, NULL);
		}
	}
	ifmedia_add(&sc->media, IFM_ETHER | IFM_AUTO, 0, NULL);
	ifmedia_set(&sc->media, IFM_ETHER | IFM_AUTO);
}

/*
 * Workaround for SmartSpeed on 82541 and 82547 controllers
 */
static void
emx_smartspeed(struct emx_softc *sc)
{
	uint16_t phy_tmp;

	if (sc->link_active || sc->hw.phy.type != e1000_phy_igp ||
	    sc->hw.mac.autoneg == 0 ||
	    (sc->hw.phy.autoneg_advertised & ADVERTISE_1000_FULL) == 0)
		return;

	if (sc->smartspeed == 0) {
		/*
		 * If Master/Slave config fault is asserted twice,
		 * we assume back-to-back
		 */
		e1000_read_phy_reg(&sc->hw, PHY_1000T_STATUS, &phy_tmp);
		if (!(phy_tmp & SR_1000T_MS_CONFIG_FAULT))
			return;
		e1000_read_phy_reg(&sc->hw, PHY_1000T_STATUS, &phy_tmp);
		if (phy_tmp & SR_1000T_MS_CONFIG_FAULT) {
			e1000_read_phy_reg(&sc->hw,
			    PHY_1000T_CTRL, &phy_tmp);
			if (phy_tmp & CR_1000T_MS_ENABLE) {
				phy_tmp &= ~CR_1000T_MS_ENABLE;
				e1000_write_phy_reg(&sc->hw,
				    PHY_1000T_CTRL, phy_tmp);
				sc->smartspeed++;
				if (sc->hw.mac.autoneg &&
				    !e1000_phy_setup_autoneg(&sc->hw) &&
				    !e1000_read_phy_reg(&sc->hw,
				     PHY_CONTROL, &phy_tmp)) {
					phy_tmp |= MII_CR_AUTO_NEG_EN |
						   MII_CR_RESTART_AUTO_NEG;
					e1000_write_phy_reg(&sc->hw,
					    PHY_CONTROL, phy_tmp);
				}
			}
		}
		return;
	} else if (sc->smartspeed == EMX_SMARTSPEED_DOWNSHIFT) {
		/* If still no link, perhaps using 2/3 pair cable */
		e1000_read_phy_reg(&sc->hw, PHY_1000T_CTRL, &phy_tmp);
		phy_tmp |= CR_1000T_MS_ENABLE;
		e1000_write_phy_reg(&sc->hw, PHY_1000T_CTRL, phy_tmp);
		if (sc->hw.mac.autoneg &&
		    !e1000_phy_setup_autoneg(&sc->hw) &&
		    !e1000_read_phy_reg(&sc->hw, PHY_CONTROL, &phy_tmp)) {
			phy_tmp |= MII_CR_AUTO_NEG_EN | MII_CR_RESTART_AUTO_NEG;
			e1000_write_phy_reg(&sc->hw, PHY_CONTROL, phy_tmp);
		}
	}

	/* Restart process after EMX_SMARTSPEED_MAX iterations */
	if (sc->smartspeed++ == EMX_SMARTSPEED_MAX)
		sc->smartspeed = 0;
}

static int
emx_create_tx_ring(struct emx_softc *sc)
{
	device_t dev = sc->dev;
	struct emx_txbuf *tx_buffer;
	int error, i, tsize;

	/*
	 * Validate number of transmit descriptors.  It must not exceed
	 * hardware maximum, and must be multiple of E1000_DBA_ALIGN.
	 */
	if ((emx_txd * sizeof(struct e1000_tx_desc)) % EMX_DBA_ALIGN != 0 ||
	    emx_txd > EMX_MAX_TXD || emx_txd < EMX_MIN_TXD) {
		device_printf(dev, "Using %d TX descriptors instead of %d!\n",
		    EMX_DEFAULT_TXD, emx_txd);
		sc->num_tx_desc = EMX_DEFAULT_TXD;
	} else {
		sc->num_tx_desc = emx_txd;
	}

	/*
	 * Allocate Transmit Descriptor ring
	 */
	tsize = roundup2(sc->num_tx_desc * sizeof(struct e1000_tx_desc),
			 EMX_DBA_ALIGN);
	sc->tx_desc_base = bus_dmamem_coherent_any(sc->parent_dtag,
				EMX_DBA_ALIGN, tsize, BUS_DMA_WAITOK,
				&sc->tx_desc_dtag, &sc->tx_desc_dmap,
				&sc->tx_desc_paddr);
	if (sc->tx_desc_base == NULL) {
		device_printf(dev, "Unable to allocate tx_desc memory\n");
		return ENOMEM;
	}

	sc->tx_buf = kmalloc(sizeof(struct emx_txbuf) * sc->num_tx_desc,
			     M_DEVBUF, M_WAITOK | M_ZERO);

	/*
	 * Create DMA tags for tx buffers
	 */
	error = bus_dma_tag_create(sc->parent_dtag, /* parent */
			1, 0,			/* alignment, bounds */
			BUS_SPACE_MAXADDR,	/* lowaddr */
			BUS_SPACE_MAXADDR,	/* highaddr */
			NULL, NULL,		/* filter, filterarg */
			EMX_TSO_SIZE,		/* maxsize */
			EMX_MAX_SCATTER,	/* nsegments */
			EMX_MAX_SEGSIZE,	/* maxsegsize */
			BUS_DMA_WAITOK | BUS_DMA_ALLOCNOW |
			BUS_DMA_ONEBPAGE,	/* flags */
			&sc->txtag);
	if (error) {
		device_printf(dev, "Unable to allocate TX DMA tag\n");
		kfree(sc->tx_buf, M_DEVBUF);
		sc->tx_buf = NULL;
		return error;
	}

	/*
	 * Create DMA maps for tx buffers
	 */
	for (i = 0; i < sc->num_tx_desc; i++) {
		tx_buffer = &sc->tx_buf[i];

		error = bus_dmamap_create(sc->txtag,
					  BUS_DMA_WAITOK | BUS_DMA_ONEBPAGE,
					  &tx_buffer->map);
		if (error) {
			device_printf(dev, "Unable to create TX DMA map\n");
			emx_destroy_tx_ring(sc, i);
			return error;
		}
	}
	return (0);
}

static void
emx_init_tx_ring(struct emx_softc *sc)
{
	/* Clear the old ring contents */
	bzero(sc->tx_desc_base,
	      sizeof(struct e1000_tx_desc) * sc->num_tx_desc);

	/* Reset state */
	sc->next_avail_tx_desc = 0;
	sc->next_tx_to_clean = 0;
	sc->num_tx_desc_avail = sc->num_tx_desc;
}

static void
emx_init_tx_unit(struct emx_softc *sc)
{
	uint32_t tctl, tarc, tipg = 0;
	uint64_t bus_addr;

	/* Setup the Base and Length of the Tx Descriptor Ring */
	bus_addr = sc->tx_desc_paddr;
	E1000_WRITE_REG(&sc->hw, E1000_TDLEN(0),
	    sc->num_tx_desc * sizeof(struct e1000_tx_desc));
	E1000_WRITE_REG(&sc->hw, E1000_TDBAH(0),
	    (uint32_t)(bus_addr >> 32));
	E1000_WRITE_REG(&sc->hw, E1000_TDBAL(0),
	    (uint32_t)bus_addr);
	/* Setup the HW Tx Head and Tail descriptor pointers */
	E1000_WRITE_REG(&sc->hw, E1000_TDT(0), 0);
	E1000_WRITE_REG(&sc->hw, E1000_TDH(0), 0);

	/* Set the default values for the Tx Inter Packet Gap timer */
	switch (sc->hw.mac.type) {
	case e1000_80003es2lan:
		tipg = DEFAULT_82543_TIPG_IPGR1;
		tipg |= DEFAULT_80003ES2LAN_TIPG_IPGR2 <<
		    E1000_TIPG_IPGR2_SHIFT;
		break;

	default:
		if (sc->hw.phy.media_type == e1000_media_type_fiber ||
		    sc->hw.phy.media_type == e1000_media_type_internal_serdes)
			tipg = DEFAULT_82543_TIPG_IPGT_FIBER;
		else
			tipg = DEFAULT_82543_TIPG_IPGT_COPPER;
		tipg |= DEFAULT_82543_TIPG_IPGR1 << E1000_TIPG_IPGR1_SHIFT;
		tipg |= DEFAULT_82543_TIPG_IPGR2 << E1000_TIPG_IPGR2_SHIFT;
		break;
	}

	E1000_WRITE_REG(&sc->hw, E1000_TIPG, tipg);

	/* NOTE: 0 is not allowed for TIDV */
	E1000_WRITE_REG(&sc->hw, E1000_TIDV, 1);
	E1000_WRITE_REG(&sc->hw, E1000_TADV, 0);

	if (sc->hw.mac.type == e1000_82571 ||
	    sc->hw.mac.type == e1000_82572) {
		tarc = E1000_READ_REG(&sc->hw, E1000_TARC(0));
		tarc |= EMX_TARC_SPEED_MODE;
		E1000_WRITE_REG(&sc->hw, E1000_TARC(0), tarc);
	} else if (sc->hw.mac.type == e1000_80003es2lan) {
		tarc = E1000_READ_REG(&sc->hw, E1000_TARC(0));
		tarc |= 1;
		E1000_WRITE_REG(&sc->hw, E1000_TARC(0), tarc);
		tarc = E1000_READ_REG(&sc->hw, E1000_TARC(1));
		tarc |= 1;
		E1000_WRITE_REG(&sc->hw, E1000_TARC(1), tarc);
	}

	/* Program the Transmit Control Register */
	tctl = E1000_READ_REG(&sc->hw, E1000_TCTL);
	tctl &= ~E1000_TCTL_CT;
	tctl |= E1000_TCTL_PSP | E1000_TCTL_RTLC | E1000_TCTL_EN |
		(E1000_COLLISION_THRESHOLD << E1000_CT_SHIFT);
	tctl |= E1000_TCTL_MULR;

	/* This write will effectively turn on the transmit unit. */
	E1000_WRITE_REG(&sc->hw, E1000_TCTL, tctl);
}

static void
emx_destroy_tx_ring(struct emx_softc *sc, int ndesc)
{
	struct emx_txbuf *tx_buffer;
	int i;

	/* Free Transmit Descriptor ring */
	if (sc->tx_desc_base) {
		bus_dmamap_unload(sc->tx_desc_dtag, sc->tx_desc_dmap);
		bus_dmamem_free(sc->tx_desc_dtag, sc->tx_desc_base,
				sc->tx_desc_dmap);
		bus_dma_tag_destroy(sc->tx_desc_dtag);

		sc->tx_desc_base = NULL;
	}

	if (sc->tx_buf == NULL)
		return;

	for (i = 0; i < ndesc; i++) {
		tx_buffer = &sc->tx_buf[i];

		KKASSERT(tx_buffer->m_head == NULL);
		bus_dmamap_destroy(sc->txtag, tx_buffer->map);
	}
	bus_dma_tag_destroy(sc->txtag);

	kfree(sc->tx_buf, M_DEVBUF);
	sc->tx_buf = NULL;
}

/*
 * The offload context needs to be set when we transfer the first
 * packet of a particular protocol (TCP/UDP).  This routine has been
 * enhanced to deal with inserted VLAN headers.
 *
 * If the new packet's ether header length, ip header length and
 * csum offloading type are same as the previous packet, we should
 * avoid allocating a new csum context descriptor; mainly to take
 * advantage of the pipeline effect of the TX data read request.
 *
 * This function returns number of TX descrptors allocated for
 * csum context.
 */
static int
emx_txcsum(struct emx_softc *sc, struct mbuf *mp,
	   uint32_t *txd_upper, uint32_t *txd_lower)
{
	struct e1000_context_desc *TXD;
	struct emx_txbuf *tx_buffer;
	struct ether_vlan_header *eh;
	struct ip *ip;
	int curr_txd, ehdrlen, csum_flags;
	uint32_t cmd, hdr_len, ip_hlen;
	uint16_t etype;

	/*
	 * Determine where frame payload starts.
	 * Jump over vlan headers if already present,
	 * helpful for QinQ too.
	 */
	KASSERT(mp->m_len >= ETHER_HDR_LEN,
		("emx_txcsum_pullup is not called (eh)?\n"));
	eh = mtod(mp, struct ether_vlan_header *);
	if (eh->evl_encap_proto == htons(ETHERTYPE_VLAN)) {
		KASSERT(mp->m_len >= ETHER_HDR_LEN + EVL_ENCAPLEN,
			("emx_txcsum_pullup is not called (evh)?\n"));
		etype = ntohs(eh->evl_proto);
		ehdrlen = ETHER_HDR_LEN + EVL_ENCAPLEN;
	} else {
		etype = ntohs(eh->evl_encap_proto);
		ehdrlen = ETHER_HDR_LEN;
	}

	/*
	 * We only support TCP/UDP for IPv4 for the moment.
	 * TODO: Support SCTP too when it hits the tree.
	 */
	if (etype != ETHERTYPE_IP)
		return 0;

	KASSERT(mp->m_len >= ehdrlen + EMX_IPVHL_SIZE,
		("emx_txcsum_pullup is not called (eh+ip_vhl)?\n"));

	/* NOTE: We could only safely access ip.ip_vhl part */
	ip = (struct ip *)(mp->m_data + ehdrlen);
	ip_hlen = ip->ip_hl << 2;

	csum_flags = mp->m_pkthdr.csum_flags & EMX_CSUM_FEATURES;

	if (sc->csum_ehlen == ehdrlen && sc->csum_iphlen == ip_hlen &&
	    sc->csum_flags == csum_flags) {
		/*
		 * Same csum offload context as the previous packets;
		 * just return.
		 */
		*txd_upper = sc->csum_txd_upper;
		*txd_lower = sc->csum_txd_lower;
		return 0;
	}

	/*
	 * Setup a new csum offload context.
	 */

	curr_txd = sc->next_avail_tx_desc;
	tx_buffer = &sc->tx_buf[curr_txd];
	TXD = (struct e1000_context_desc *)&sc->tx_desc_base[curr_txd];

	cmd = 0;

	/* Setup of IP header checksum. */
	if (csum_flags & CSUM_IP) {
		/*
		 * Start offset for header checksum calculation.
		 * End offset for header checksum calculation.
		 * Offset of place to put the checksum.
		 */
		TXD->lower_setup.ip_fields.ipcss = ehdrlen;
		TXD->lower_setup.ip_fields.ipcse =
		    htole16(ehdrlen + ip_hlen - 1);
		TXD->lower_setup.ip_fields.ipcso =
		    ehdrlen + offsetof(struct ip, ip_sum);
		cmd |= E1000_TXD_CMD_IP;
		*txd_upper |= E1000_TXD_POPTS_IXSM << 8;
	}
	hdr_len = ehdrlen + ip_hlen;

	if (csum_flags & CSUM_TCP) {
		/*
		 * Start offset for payload checksum calculation.
		 * End offset for payload checksum calculation.
		 * Offset of place to put the checksum.
		 */
		TXD->upper_setup.tcp_fields.tucss = hdr_len;
		TXD->upper_setup.tcp_fields.tucse = htole16(0);
		TXD->upper_setup.tcp_fields.tucso =
		    hdr_len + offsetof(struct tcphdr, th_sum);
		cmd |= E1000_TXD_CMD_TCP;
		*txd_upper |= E1000_TXD_POPTS_TXSM << 8;
	} else if (csum_flags & CSUM_UDP) {
		/*
		 * Start offset for header checksum calculation.
		 * End offset for header checksum calculation.
		 * Offset of place to put the checksum.
		 */
		TXD->upper_setup.tcp_fields.tucss = hdr_len;
		TXD->upper_setup.tcp_fields.tucse = htole16(0);
		TXD->upper_setup.tcp_fields.tucso =
		    hdr_len + offsetof(struct udphdr, uh_sum);
		*txd_upper |= E1000_TXD_POPTS_TXSM << 8;
	}

	*txd_lower = E1000_TXD_CMD_DEXT |	/* Extended descr type */
		     E1000_TXD_DTYP_D;		/* Data descr */

	/* Save the information for this csum offloading context */
	sc->csum_ehlen = ehdrlen;
	sc->csum_iphlen = ip_hlen;
	sc->csum_flags = csum_flags;
	sc->csum_txd_upper = *txd_upper;
	sc->csum_txd_lower = *txd_lower;

	TXD->tcp_seg_setup.data = htole32(0);
	TXD->cmd_and_length =
	    htole32(E1000_TXD_CMD_IFCS | E1000_TXD_CMD_DEXT | cmd);

	if (++curr_txd == sc->num_tx_desc)
		curr_txd = 0;

	KKASSERT(sc->num_tx_desc_avail > 0);
	sc->num_tx_desc_avail--;

	sc->next_avail_tx_desc = curr_txd;
	return 1;
}

static int
emx_txcsum_pullup(struct emx_softc *sc, struct mbuf **m0)
{
	struct mbuf *m = *m0;
	struct ether_header *eh;
	int len;

	sc->tx_csum_try_pullup++;

	len = ETHER_HDR_LEN + EMX_IPVHL_SIZE;

	if (__predict_false(!M_WRITABLE(m))) {
		if (__predict_false(m->m_len < ETHER_HDR_LEN)) {
			sc->tx_csum_drop1++;
			m_freem(m);
			*m0 = NULL;
			return ENOBUFS;
		}
		eh = mtod(m, struct ether_header *);

		if (eh->ether_type == htons(ETHERTYPE_VLAN))
			len += EVL_ENCAPLEN;

		if (m->m_len < len) {
			sc->tx_csum_drop2++;
			m_freem(m);
			*m0 = NULL;
			return ENOBUFS;
		}
		return 0;
	}

	if (__predict_false(m->m_len < ETHER_HDR_LEN)) {
		sc->tx_csum_pullup1++;
		m = m_pullup(m, ETHER_HDR_LEN);
		if (m == NULL) {
			sc->tx_csum_pullup1_failed++;
			*m0 = NULL;
			return ENOBUFS;
		}
		*m0 = m;
	}
	eh = mtod(m, struct ether_header *);

	if (eh->ether_type == htons(ETHERTYPE_VLAN))
		len += EVL_ENCAPLEN;

	if (m->m_len < len) {
		sc->tx_csum_pullup2++;
		m = m_pullup(m, len);
		if (m == NULL) {
			sc->tx_csum_pullup2_failed++;
			*m0 = NULL;
			return ENOBUFS;
		}
		*m0 = m;
	}
	return 0;
}

static void
emx_txeof(struct emx_softc *sc)
{
	struct ifnet *ifp = &sc->arpcom.ac_if;
	struct emx_txbuf *tx_buffer;
	int first, num_avail;

	if (sc->tx_dd_head == sc->tx_dd_tail)
		return;

	if (sc->num_tx_desc_avail == sc->num_tx_desc)
		return;

	num_avail = sc->num_tx_desc_avail;
	first = sc->next_tx_to_clean;

	while (sc->tx_dd_head != sc->tx_dd_tail) {
		int dd_idx = sc->tx_dd[sc->tx_dd_head];
		struct e1000_tx_desc *tx_desc;

		tx_desc = &sc->tx_desc_base[dd_idx];
		if (tx_desc->upper.fields.status & E1000_TXD_STAT_DD) {
			EMX_INC_TXDD_IDX(sc->tx_dd_head);

			if (++dd_idx == sc->num_tx_desc)
				dd_idx = 0;

			while (first != dd_idx) {
				logif(pkt_txclean);

				num_avail++;

				tx_buffer = &sc->tx_buf[first];
				if (tx_buffer->m_head) {
					ifp->if_opackets++;
					bus_dmamap_unload(sc->txtag,
							  tx_buffer->map);
					m_freem(tx_buffer->m_head);
					tx_buffer->m_head = NULL;
				}

				if (++first == sc->num_tx_desc)
					first = 0;
			}
		} else {
			break;
		}
	}
	sc->next_tx_to_clean = first;
	sc->num_tx_desc_avail = num_avail;

	if (sc->tx_dd_head == sc->tx_dd_tail) {
		sc->tx_dd_head = 0;
		sc->tx_dd_tail = 0;
	}

	if (!EMX_IS_OACTIVE(sc)) {
		ifp->if_flags &= ~IFF_OACTIVE;

		/* All clean, turn off the timer */
		if (sc->num_tx_desc_avail == sc->num_tx_desc)
			ifp->if_timer = 0;
	}
}

static void
emx_tx_collect(struct emx_softc *sc)
{
	struct ifnet *ifp = &sc->arpcom.ac_if;
	struct emx_txbuf *tx_buffer;
	int tdh, first, num_avail, dd_idx = -1;

	if (sc->num_tx_desc_avail == sc->num_tx_desc)
		return;

	tdh = E1000_READ_REG(&sc->hw, E1000_TDH(0));
	if (tdh == sc->next_tx_to_clean)
		return;

	if (sc->tx_dd_head != sc->tx_dd_tail)
		dd_idx = sc->tx_dd[sc->tx_dd_head];

	num_avail = sc->num_tx_desc_avail;
	first = sc->next_tx_to_clean;

	while (first != tdh) {
		logif(pkt_txclean);

		num_avail++;

		tx_buffer = &sc->tx_buf[first];
		if (tx_buffer->m_head) {
			ifp->if_opackets++;
			bus_dmamap_unload(sc->txtag,
					  tx_buffer->map);
			m_freem(tx_buffer->m_head);
			tx_buffer->m_head = NULL;
		}

		if (first == dd_idx) {
			EMX_INC_TXDD_IDX(sc->tx_dd_head);
			if (sc->tx_dd_head == sc->tx_dd_tail) {
				sc->tx_dd_head = 0;
				sc->tx_dd_tail = 0;
				dd_idx = -1;
			} else {
				dd_idx = sc->tx_dd[sc->tx_dd_head];
			}
		}

		if (++first == sc->num_tx_desc)
			first = 0;
	}
	sc->next_tx_to_clean = first;
	sc->num_tx_desc_avail = num_avail;

	if (!EMX_IS_OACTIVE(sc)) {
		ifp->if_flags &= ~IFF_OACTIVE;

		/* All clean, turn off the timer */
		if (sc->num_tx_desc_avail == sc->num_tx_desc)
			ifp->if_timer = 0;
	}
}

/*
 * When Link is lost sometimes there is work still in the TX ring
 * which will result in a watchdog, rather than allow that do an
 * attempted cleanup and then reinit here.  Note that this has been
 * seens mostly with fiber adapters.
 */
static void
emx_tx_purge(struct emx_softc *sc)
{
	struct ifnet *ifp = &sc->arpcom.ac_if;

	if (!sc->link_active && ifp->if_timer) {
		emx_tx_collect(sc);
		if (ifp->if_timer) {
			if_printf(ifp, "Link lost, TX pending, reinit\n");
			ifp->if_timer = 0;
			emx_init(sc);
		}
	}
}

static int
emx_newbuf(struct emx_softc *sc, struct emx_rxdata *rdata, int i, int init)
{
	struct mbuf *m;
	bus_dma_segment_t seg;
	bus_dmamap_t map;
	struct emx_rxbuf *rx_buffer;
	int error, nseg;

	m = m_getcl(init ? MB_WAIT : MB_DONTWAIT, MT_DATA, M_PKTHDR);
	if (m == NULL) {
		rdata->mbuf_cluster_failed++;
		if (init) {
			if_printf(&sc->arpcom.ac_if,
				  "Unable to allocate RX mbuf\n");
		}
		return (ENOBUFS);
	}
	m->m_len = m->m_pkthdr.len = MCLBYTES;

	if (sc->max_frame_size <= MCLBYTES - ETHER_ALIGN)
		m_adj(m, ETHER_ALIGN);

	error = bus_dmamap_load_mbuf_segment(rdata->rxtag,
			rdata->rx_sparemap, m,
			&seg, 1, &nseg, BUS_DMA_NOWAIT);
	if (error) {
		m_freem(m);
		if (init) {
			if_printf(&sc->arpcom.ac_if,
				  "Unable to load RX mbuf\n");
		}
		return (error);
	}

	rx_buffer = &rdata->rx_buf[i];
	if (rx_buffer->m_head != NULL)
		bus_dmamap_unload(rdata->rxtag, rx_buffer->map);

	map = rx_buffer->map;
	rx_buffer->map = rdata->rx_sparemap;
	rdata->rx_sparemap = map;

	rx_buffer->m_head = m;
	rx_buffer->paddr = seg.ds_addr;

	emx_setup_rxdesc(&rdata->rx_desc[i], rx_buffer);
	return (0);
}

static int
emx_create_rx_ring(struct emx_softc *sc, struct emx_rxdata *rdata)
{
	device_t dev = sc->dev;
	struct emx_rxbuf *rx_buffer;
	int i, error, rsize;

	/*
	 * Validate number of receive descriptors.  It must not exceed
	 * hardware maximum, and must be multiple of E1000_DBA_ALIGN.
	 */
	if ((emx_rxd * sizeof(emx_rxdesc_t)) % EMX_DBA_ALIGN != 0 ||
	    emx_rxd > EMX_MAX_RXD || emx_rxd < EMX_MIN_RXD) {
		device_printf(dev, "Using %d RX descriptors instead of %d!\n",
		    EMX_DEFAULT_RXD, emx_rxd);
		rdata->num_rx_desc = EMX_DEFAULT_RXD;
	} else {
		rdata->num_rx_desc = emx_rxd;
	}

	/*
	 * Allocate Receive Descriptor ring
	 */
	rsize = roundup2(rdata->num_rx_desc * sizeof(emx_rxdesc_t),
			 EMX_DBA_ALIGN);
	rdata->rx_desc = bus_dmamem_coherent_any(sc->parent_dtag,
				EMX_DBA_ALIGN, rsize, BUS_DMA_WAITOK,
				&rdata->rx_desc_dtag, &rdata->rx_desc_dmap,
				&rdata->rx_desc_paddr);
	if (rdata->rx_desc == NULL) {
		device_printf(dev, "Unable to allocate rx_desc memory\n");
		return ENOMEM;
	}

	rdata->rx_buf = kmalloc(sizeof(struct emx_rxbuf) * rdata->num_rx_desc,
				M_DEVBUF, M_WAITOK | M_ZERO);

	/*
	 * Create DMA tag for rx buffers
	 */
	error = bus_dma_tag_create(sc->parent_dtag, /* parent */
			1, 0,			/* alignment, bounds */
			BUS_SPACE_MAXADDR,	/* lowaddr */
			BUS_SPACE_MAXADDR,	/* highaddr */
			NULL, NULL,		/* filter, filterarg */
			MCLBYTES,		/* maxsize */
			1,			/* nsegments */
			MCLBYTES,		/* maxsegsize */
			BUS_DMA_WAITOK | BUS_DMA_ALLOCNOW, /* flags */
			&rdata->rxtag);
	if (error) {
		device_printf(dev, "Unable to allocate RX DMA tag\n");
		kfree(rdata->rx_buf, M_DEVBUF);
		rdata->rx_buf = NULL;
		return error;
	}

	/*
	 * Create spare DMA map for rx buffers
	 */
	error = bus_dmamap_create(rdata->rxtag, BUS_DMA_WAITOK,
				  &rdata->rx_sparemap);
	if (error) {
		device_printf(dev, "Unable to create spare RX DMA map\n");
		bus_dma_tag_destroy(rdata->rxtag);
		kfree(rdata->rx_buf, M_DEVBUF);
		rdata->rx_buf = NULL;
		return error;
	}

	/*
	 * Create DMA maps for rx buffers
	 */
	for (i = 0; i < rdata->num_rx_desc; i++) {
		rx_buffer = &rdata->rx_buf[i];

		error = bus_dmamap_create(rdata->rxtag, BUS_DMA_WAITOK,
					  &rx_buffer->map);
		if (error) {
			device_printf(dev, "Unable to create RX DMA map\n");
			emx_destroy_rx_ring(sc, rdata, i);
			return error;
		}
	}
	return (0);
}

static void
emx_free_rx_ring(struct emx_softc *sc, struct emx_rxdata *rdata)
{
	int i;

	for (i = 0; i < rdata->num_rx_desc; i++) {
		struct emx_rxbuf *rx_buffer = &rdata->rx_buf[i];

		if (rx_buffer->m_head != NULL) {
			bus_dmamap_unload(rdata->rxtag, rx_buffer->map);
			m_freem(rx_buffer->m_head);
			rx_buffer->m_head = NULL;
		}
	}

	if (rdata->fmp != NULL)
		m_freem(rdata->fmp);
	rdata->fmp = NULL;
	rdata->lmp = NULL;
}

static int
emx_init_rx_ring(struct emx_softc *sc, struct emx_rxdata *rdata)
{
	int i, error;

	/* Reset descriptor ring */
	bzero(rdata->rx_desc, sizeof(emx_rxdesc_t) * rdata->num_rx_desc);

	/* Allocate new ones. */
	for (i = 0; i < rdata->num_rx_desc; i++) {
		error = emx_newbuf(sc, rdata, i, 1);
		if (error)
			return (error);
	}

	/* Setup our descriptor pointers */
	rdata->next_rx_desc_to_check = 0;

	return (0);
}

static void
emx_init_rx_unit(struct emx_softc *sc)
{
	struct ifnet *ifp = &sc->arpcom.ac_if;
	uint64_t bus_addr;
	uint32_t rctl, rxcsum, rfctl, key, reta;
	int i;

	/*
	 * Make sure receives are disabled while setting
	 * up the descriptor ring
	 */
	rctl = E1000_READ_REG(&sc->hw, E1000_RCTL);
	E1000_WRITE_REG(&sc->hw, E1000_RCTL, rctl & ~E1000_RCTL_EN);

	/*
	 * Set the interrupt throttling rate. Value is calculated
	 * as ITR = 1 / (INT_THROTTLE_CEIL * 256ns)
	 */
	if (sc->int_throttle_ceil) {
		E1000_WRITE_REG(&sc->hw, E1000_ITR,
			1000000000 / 256 / sc->int_throttle_ceil);
	} else {
		E1000_WRITE_REG(&sc->hw, E1000_ITR, 0);
	}

	/* Use extended RX descriptor */
	rfctl = E1000_RFCTL_EXTEN;

	/* Disable accelerated ackknowledge */
	if (sc->hw.mac.type == e1000_82574)
		rfctl |= E1000_RFCTL_ACK_DIS;

	E1000_WRITE_REG(&sc->hw, E1000_RFCTL, rfctl);

	/* Setup the Base and Length of the Rx Descriptor Ring */
	for (i = 0; i < sc->rx_ring_inuse; ++i) {
		struct emx_rxdata *rdata = &sc->rx_data[i];

		bus_addr = rdata->rx_desc_paddr;
		E1000_WRITE_REG(&sc->hw, E1000_RDLEN(i),
		    rdata->num_rx_desc * sizeof(emx_rxdesc_t));
		E1000_WRITE_REG(&sc->hw, E1000_RDBAH(i),
		    (uint32_t)(bus_addr >> 32));
		E1000_WRITE_REG(&sc->hw, E1000_RDBAL(i),
		    (uint32_t)bus_addr);
	}

	/* Setup the Receive Control Register */
	rctl &= ~(3 << E1000_RCTL_MO_SHIFT);
	rctl |= E1000_RCTL_EN | E1000_RCTL_BAM | E1000_RCTL_LBM_NO |
		E1000_RCTL_RDMTS_HALF | E1000_RCTL_SECRC |
		(sc->hw.mac.mc_filter_type << E1000_RCTL_MO_SHIFT);

	/* Make sure VLAN Filters are off */
	rctl &= ~E1000_RCTL_VFE;

	/* Don't store bad paket */
	rctl &= ~E1000_RCTL_SBP;

	/* MCLBYTES */
	rctl |= E1000_RCTL_SZ_2048;

	if (ifp->if_mtu > ETHERMTU)
		rctl |= E1000_RCTL_LPE;
	else
		rctl &= ~E1000_RCTL_LPE;

	/*
	 * Receive Checksum Offload for TCP and UDP
	 *
	 * Checksum offloading is also enabled if multiple receive
	 * queue is to be supported, since we need it to figure out
	 * packet type.
	 */
	if (ifp->if_capenable & (IFCAP_RSS | IFCAP_RXCSUM)) {
		rxcsum = E1000_READ_REG(&sc->hw, E1000_RXCSUM);

		/*
		 * NOTE:
		 * PCSD must be enabled to enable multiple
		 * receive queues.
		 */
		rxcsum |= E1000_RXCSUM_IPOFL | E1000_RXCSUM_TUOFL |
			  E1000_RXCSUM_PCSD;
		E1000_WRITE_REG(&sc->hw, E1000_RXCSUM, rxcsum);
	}

	/*
	 * Configure multiple receive queue (RSS)
	 */
	if (ifp->if_capenable & IFCAP_RSS) {
		/*
		 * NOTE:
		 * When we reach here, RSS has already been disabled
		 * in emx_stop(), so we could safely configure RSS key
		 * and redirect table.
		 */

		/*
		 * Configure RSS key
		 */
		key = 0x5a6d5a6d; /* XXX */
		for (i = 0; i < EMX_NRSSRK; ++i)
			E1000_WRITE_REG(&sc->hw, E1000_RSSRK(i), key);

		/*
		 * Configure RSS redirect table
		 */
		reta = 0x80008000;
		for (i = 0; i < EMX_NRETA; ++i)
			E1000_WRITE_REG(&sc->hw, E1000_RETA(i), reta);

		/*
		 * Enable multiple receive queues.
		 * Enable IPv4 RSS standard hash functions.
		 * Disable RSS interrupt.
		 */
		E1000_WRITE_REG(&sc->hw, E1000_MRQC,
				E1000_MRQC_ENABLE_RSS_2Q |
				E1000_MRQC_RSS_FIELD_IPV4_TCP |
				E1000_MRQC_RSS_FIELD_IPV4);
	}

	/*
	 * XXX TEMPORARY WORKAROUND: on some systems with 82573
	 * long latencies are observed, like Lenovo X60. This
	 * change eliminates the problem, but since having positive
	 * values in RDTR is a known source of problems on other
	 * platforms another solution is being sought.
	 */
	if (emx_82573_workaround && sc->hw.mac.type == e1000_82573) {
		E1000_WRITE_REG(&sc->hw, E1000_RADV, EMX_RADV_82573);
		E1000_WRITE_REG(&sc->hw, E1000_RDTR, EMX_RDTR_82573);
	}

	/*
	 * Setup the HW Rx Head and Tail Descriptor Pointers
	 */
	for (i = 0; i < sc->rx_ring_inuse; ++i) {
		E1000_WRITE_REG(&sc->hw, E1000_RDH(i), 0);
		E1000_WRITE_REG(&sc->hw, E1000_RDT(i),
		    sc->rx_data[i].num_rx_desc - 1);
	}

	/* Enable Receives */
	E1000_WRITE_REG(&sc->hw, E1000_RCTL, rctl);
}

static void
emx_destroy_rx_ring(struct emx_softc *sc, struct emx_rxdata *rdata, int ndesc)
{
	struct emx_rxbuf *rx_buffer;
	int i;

	/* Free Receive Descriptor ring */
	if (rdata->rx_desc) {
		bus_dmamap_unload(rdata->rx_desc_dtag, rdata->rx_desc_dmap);
		bus_dmamem_free(rdata->rx_desc_dtag, rdata->rx_desc,
				rdata->rx_desc_dmap);
		bus_dma_tag_destroy(rdata->rx_desc_dtag);

		rdata->rx_desc = NULL;
	}

	if (rdata->rx_buf == NULL)
		return;

	for (i = 0; i < ndesc; i++) {
		rx_buffer = &rdata->rx_buf[i];

		KKASSERT(rx_buffer->m_head == NULL);
		bus_dmamap_destroy(rdata->rxtag, rx_buffer->map);
	}
	bus_dmamap_destroy(rdata->rxtag, rdata->rx_sparemap);
	bus_dma_tag_destroy(rdata->rxtag);

	kfree(rdata->rx_buf, M_DEVBUF);
	rdata->rx_buf = NULL;
}

static void
emx_rxeof(struct emx_softc *sc, int ring_idx, int count)
{
	struct emx_rxdata *rdata = &sc->rx_data[ring_idx];
	struct ifnet *ifp = &sc->arpcom.ac_if;
	uint32_t staterr;
	emx_rxdesc_t *current_desc;
	struct mbuf *mp;
	int i;
	struct mbuf_chain chain[MAXCPU];

	i = rdata->next_rx_desc_to_check;
	current_desc = &rdata->rx_desc[i];
	staterr = le32toh(current_desc->rxd_staterr);

	if (!(staterr & E1000_RXD_STAT_DD))
		return;

	ether_input_chain_init(chain);

	while ((staterr & E1000_RXD_STAT_DD) && count != 0) {
		struct emx_rxbuf *rx_buf = &rdata->rx_buf[i];
		struct mbuf *m = NULL;
		int eop, len;

		logif(pkt_receive);

		mp = rx_buf->m_head;

		/*
		 * Can't defer bus_dmamap_sync(9) because TBI_ACCEPT
		 * needs to access the last received byte in the mbuf.
		 */
		bus_dmamap_sync(rdata->rxtag, rx_buf->map,
				BUS_DMASYNC_POSTREAD);

		len = le16toh(current_desc->rxd_length);
		if (staterr & E1000_RXD_STAT_EOP) {
			count--;
			eop = 1;
		} else {
			eop = 0;
		}

		if (!(staterr & E1000_RXDEXT_ERR_FRAME_ERR_MASK)) {
			uint16_t vlan = 0;
			uint32_t mrq, rss_hash;

			/*
			 * Save several necessary information,
			 * before emx_newbuf() destroy it.
			 */
			if ((staterr & E1000_RXD_STAT_VP) && eop)
				vlan = le16toh(current_desc->rxd_vlan);

			mrq = le32toh(current_desc->rxd_mrq);
			rss_hash = le32toh(current_desc->rxd_rss);

			EMX_RSS_DPRINTF(sc, 10,
			    "ring%d, mrq 0x%08x, rss_hash 0x%08x\n",
			    ring_idx, mrq, rss_hash);

			if (emx_newbuf(sc, rdata, i, 0) != 0) {
				ifp->if_iqdrops++;
				goto discard;
			}

			/* Assign correct length to the current fragment */
			mp->m_len = len;

			if (rdata->fmp == NULL) {
				mp->m_pkthdr.len = len;
				rdata->fmp = mp; /* Store the first mbuf */
				rdata->lmp = mp;
			} else {
				/*
				 * Chain mbuf's together
				 */
				rdata->lmp->m_next = mp;
				rdata->lmp = rdata->lmp->m_next;
				rdata->fmp->m_pkthdr.len += len;
			}

			if (eop) {
				rdata->fmp->m_pkthdr.rcvif = ifp;
				ifp->if_ipackets++;

				if (ifp->if_capenable & IFCAP_RXCSUM)
					emx_rxcsum(staterr, rdata->fmp);

				if (staterr & E1000_RXD_STAT_VP) {
					rdata->fmp->m_pkthdr.ether_vlantag =
					    vlan;
					rdata->fmp->m_flags |= M_VLANTAG;
				}
				m = rdata->fmp;
				rdata->fmp = NULL;
				rdata->lmp = NULL;

#ifdef EMX_RSS_DEBUG
				rdata->rx_pkts++;
#endif
			}
		} else {
			ifp->if_ierrors++;
discard:
			emx_setup_rxdesc(current_desc, rx_buf);
			if (rdata->fmp != NULL) {
				m_freem(rdata->fmp);
				rdata->fmp = NULL;
				rdata->lmp = NULL;
			}
			m = NULL;
		}

		if (m != NULL)
			ether_input_chain(ifp, m, chain);

		/* Advance our pointers to the next descriptor. */
		if (++i == rdata->num_rx_desc)
			i = 0;

		current_desc = &rdata->rx_desc[i];
		staterr = le32toh(current_desc->rxd_staterr);
	}
	rdata->next_rx_desc_to_check = i;

	ether_input_dispatch(chain);

	/* Advance the E1000's Receive Queue "Tail Pointer". */
	if (--i < 0)
		i = rdata->num_rx_desc - 1;
	E1000_WRITE_REG(&sc->hw, E1000_RDT(ring_idx), i);
}

static void
emx_enable_intr(struct emx_softc *sc)
{
	lwkt_serialize_handler_enable(sc->arpcom.ac_if.if_serializer);
	E1000_WRITE_REG(&sc->hw, E1000_IMS, IMS_ENABLE_MASK);
}

static void
emx_disable_intr(struct emx_softc *sc)
{
	E1000_WRITE_REG(&sc->hw, E1000_IMC, 0xffffffff);
	lwkt_serialize_handler_disable(sc->arpcom.ac_if.if_serializer);
}

/*
 * Bit of a misnomer, what this really means is
 * to enable OS management of the system... aka
 * to disable special hardware management features 
 */
static void
emx_get_mgmt(struct emx_softc *sc)
{
	/* A shared code workaround */
	if (sc->has_manage) {
		int manc2h = E1000_READ_REG(&sc->hw, E1000_MANC2H);
		int manc = E1000_READ_REG(&sc->hw, E1000_MANC);

		/* disable hardware interception of ARP */
		manc &= ~(E1000_MANC_ARP_EN);

                /* enable receiving management packets to the host */
		manc |= E1000_MANC_EN_MNG2HOST;
#define E1000_MNG2HOST_PORT_623 (1 << 5)
#define E1000_MNG2HOST_PORT_664 (1 << 6)
		manc2h |= E1000_MNG2HOST_PORT_623;
		manc2h |= E1000_MNG2HOST_PORT_664;
		E1000_WRITE_REG(&sc->hw, E1000_MANC2H, manc2h);

		E1000_WRITE_REG(&sc->hw, E1000_MANC, manc);
	}
}

/*
 * Give control back to hardware management
 * controller if there is one.
 */
static void
emx_rel_mgmt(struct emx_softc *sc)
{
	if (sc->has_manage) {
		int manc = E1000_READ_REG(&sc->hw, E1000_MANC);

		/* re-enable hardware interception of ARP */
		manc |= E1000_MANC_ARP_EN;
		manc &= ~E1000_MANC_EN_MNG2HOST;

		E1000_WRITE_REG(&sc->hw, E1000_MANC, manc);
	}
}

/*
 * emx_get_hw_control() sets {CTRL_EXT|FWSM}:DRV_LOAD bit.
 * For ASF and Pass Through versions of f/w this means that
 * the driver is loaded.  For AMT version (only with 82573)
 * of the f/w this means that the network i/f is open.
 */
static void
emx_get_hw_control(struct emx_softc *sc)
{
	uint32_t ctrl_ext, swsm;

	/* Let firmware know the driver has taken over */
	switch (sc->hw.mac.type) {
	case e1000_82573:
		swsm = E1000_READ_REG(&sc->hw, E1000_SWSM);
		E1000_WRITE_REG(&sc->hw, E1000_SWSM,
		    swsm | E1000_SWSM_DRV_LOAD);
		break;

	case e1000_82571:
	case e1000_82572:
	case e1000_80003es2lan:
		ctrl_ext = E1000_READ_REG(&sc->hw, E1000_CTRL_EXT);
		E1000_WRITE_REG(&sc->hw, E1000_CTRL_EXT,
		    ctrl_ext | E1000_CTRL_EXT_DRV_LOAD);
		break;

	default:
		break;
	}
}

/*
 * emx_rel_hw_control() resets {CTRL_EXT|FWSM}:DRV_LOAD bit.
 * For ASF and Pass Through versions of f/w this means that the
 * driver is no longer loaded.  For AMT version (only with 82573)
 * of the f/w this means that the network i/f is closed.
 */
static void
emx_rel_hw_control(struct emx_softc *sc)
{
	uint32_t ctrl_ext, swsm;

	/* Let firmware taken over control of h/w */
	switch (sc->hw.mac.type) {
	case e1000_82573:
		swsm = E1000_READ_REG(&sc->hw, E1000_SWSM);
		E1000_WRITE_REG(&sc->hw, E1000_SWSM,
		    swsm & ~E1000_SWSM_DRV_LOAD);
		break;

	case e1000_82571:
	case e1000_82572:
	case e1000_80003es2lan:
		ctrl_ext = E1000_READ_REG(&sc->hw, E1000_CTRL_EXT);
		E1000_WRITE_REG(&sc->hw, E1000_CTRL_EXT,
		    ctrl_ext & ~E1000_CTRL_EXT_DRV_LOAD);
		break;

	default:
		break;
	}
}

static int
emx_is_valid_eaddr(const uint8_t *addr)
{
	char zero_addr[ETHER_ADDR_LEN] = { 0, 0, 0, 0, 0, 0 };

	if ((addr[0] & 1) || !bcmp(addr, zero_addr, ETHER_ADDR_LEN))
		return (FALSE);

	return (TRUE);
}

/*
 * Enable PCI Wake On Lan capability
 */
void
emx_enable_wol(device_t dev)
{
	uint16_t cap, status;
	uint8_t id;

	/* First find the capabilities pointer*/
	cap = pci_read_config(dev, PCIR_CAP_PTR, 2);

	/* Read the PM Capabilities */
	id = pci_read_config(dev, cap, 1);
	if (id != PCIY_PMG)     /* Something wrong */
		return;

	/*
	 * OK, we have the power capabilities,
	 * so now get the status register
	 */
	cap += PCIR_POWER_STATUS;
	status = pci_read_config(dev, cap, 2);
	status |= PCIM_PSTAT_PME | PCIM_PSTAT_PMEENABLE;
	pci_write_config(dev, cap, status, 2);
}

static void
emx_update_stats(struct emx_softc *sc)
{
	struct ifnet *ifp = &sc->arpcom.ac_if;

	if (sc->hw.phy.media_type == e1000_media_type_copper ||
	    (E1000_READ_REG(&sc->hw, E1000_STATUS) & E1000_STATUS_LU)) {
		sc->stats.symerrs += E1000_READ_REG(&sc->hw, E1000_SYMERRS);
		sc->stats.sec += E1000_READ_REG(&sc->hw, E1000_SEC);
	}
	sc->stats.crcerrs += E1000_READ_REG(&sc->hw, E1000_CRCERRS);
	sc->stats.mpc += E1000_READ_REG(&sc->hw, E1000_MPC);
	sc->stats.scc += E1000_READ_REG(&sc->hw, E1000_SCC);
	sc->stats.ecol += E1000_READ_REG(&sc->hw, E1000_ECOL);

	sc->stats.mcc += E1000_READ_REG(&sc->hw, E1000_MCC);
	sc->stats.latecol += E1000_READ_REG(&sc->hw, E1000_LATECOL);
	sc->stats.colc += E1000_READ_REG(&sc->hw, E1000_COLC);
	sc->stats.dc += E1000_READ_REG(&sc->hw, E1000_DC);
	sc->stats.rlec += E1000_READ_REG(&sc->hw, E1000_RLEC);
	sc->stats.xonrxc += E1000_READ_REG(&sc->hw, E1000_XONRXC);
	sc->stats.xontxc += E1000_READ_REG(&sc->hw, E1000_XONTXC);
	sc->stats.xoffrxc += E1000_READ_REG(&sc->hw, E1000_XOFFRXC);
	sc->stats.xofftxc += E1000_READ_REG(&sc->hw, E1000_XOFFTXC);
	sc->stats.fcruc += E1000_READ_REG(&sc->hw, E1000_FCRUC);
	sc->stats.prc64 += E1000_READ_REG(&sc->hw, E1000_PRC64);
	sc->stats.prc127 += E1000_READ_REG(&sc->hw, E1000_PRC127);
	sc->stats.prc255 += E1000_READ_REG(&sc->hw, E1000_PRC255);
	sc->stats.prc511 += E1000_READ_REG(&sc->hw, E1000_PRC511);
	sc->stats.prc1023 += E1000_READ_REG(&sc->hw, E1000_PRC1023);
	sc->stats.prc1522 += E1000_READ_REG(&sc->hw, E1000_PRC1522);
	sc->stats.gprc += E1000_READ_REG(&sc->hw, E1000_GPRC);
	sc->stats.bprc += E1000_READ_REG(&sc->hw, E1000_BPRC);
	sc->stats.mprc += E1000_READ_REG(&sc->hw, E1000_MPRC);
	sc->stats.gptc += E1000_READ_REG(&sc->hw, E1000_GPTC);

	/* For the 64-bit byte counters the low dword must be read first. */
	/* Both registers clear on the read of the high dword */

	sc->stats.gorc += E1000_READ_REG(&sc->hw, E1000_GORCH);
	sc->stats.gotc += E1000_READ_REG(&sc->hw, E1000_GOTCH);

	sc->stats.rnbc += E1000_READ_REG(&sc->hw, E1000_RNBC);
	sc->stats.ruc += E1000_READ_REG(&sc->hw, E1000_RUC);
	sc->stats.rfc += E1000_READ_REG(&sc->hw, E1000_RFC);
	sc->stats.roc += E1000_READ_REG(&sc->hw, E1000_ROC);
	sc->stats.rjc += E1000_READ_REG(&sc->hw, E1000_RJC);

	sc->stats.tor += E1000_READ_REG(&sc->hw, E1000_TORH);
	sc->stats.tot += E1000_READ_REG(&sc->hw, E1000_TOTH);

	sc->stats.tpr += E1000_READ_REG(&sc->hw, E1000_TPR);
	sc->stats.tpt += E1000_READ_REG(&sc->hw, E1000_TPT);
	sc->stats.ptc64 += E1000_READ_REG(&sc->hw, E1000_PTC64);
	sc->stats.ptc127 += E1000_READ_REG(&sc->hw, E1000_PTC127);
	sc->stats.ptc255 += E1000_READ_REG(&sc->hw, E1000_PTC255);
	sc->stats.ptc511 += E1000_READ_REG(&sc->hw, E1000_PTC511);
	sc->stats.ptc1023 += E1000_READ_REG(&sc->hw, E1000_PTC1023);
	sc->stats.ptc1522 += E1000_READ_REG(&sc->hw, E1000_PTC1522);
	sc->stats.mptc += E1000_READ_REG(&sc->hw, E1000_MPTC);
	sc->stats.bptc += E1000_READ_REG(&sc->hw, E1000_BPTC);

	sc->stats.algnerrc += E1000_READ_REG(&sc->hw, E1000_ALGNERRC);
	sc->stats.rxerrc += E1000_READ_REG(&sc->hw, E1000_RXERRC);
	sc->stats.tncrs += E1000_READ_REG(&sc->hw, E1000_TNCRS);
	sc->stats.cexterr += E1000_READ_REG(&sc->hw, E1000_CEXTERR);
	sc->stats.tsctc += E1000_READ_REG(&sc->hw, E1000_TSCTC);
	sc->stats.tsctfc += E1000_READ_REG(&sc->hw, E1000_TSCTFC);

	ifp->if_collisions = sc->stats.colc;

	/* Rx Errors */
	ifp->if_ierrors = sc->dropped_pkts + sc->stats.rxerrc +
			  sc->stats.crcerrs + sc->stats.algnerrc +
			  sc->stats.ruc + sc->stats.roc +
			  sc->stats.mpc + sc->stats.cexterr;

	/* Tx Errors */
	ifp->if_oerrors = sc->stats.ecol + sc->stats.latecol +
			  sc->watchdog_events;
}

static void
emx_print_debug_info(struct emx_softc *sc)
{
	device_t dev = sc->dev;
	uint8_t *hw_addr = sc->hw.hw_addr;

	device_printf(dev, "Adapter hardware address = %p \n", hw_addr);
	device_printf(dev, "CTRL = 0x%x RCTL = 0x%x \n",
	    E1000_READ_REG(&sc->hw, E1000_CTRL),
	    E1000_READ_REG(&sc->hw, E1000_RCTL));
	device_printf(dev, "Packet buffer = Tx=%dk Rx=%dk \n",
	    ((E1000_READ_REG(&sc->hw, E1000_PBA) & 0xffff0000) >> 16),\
	    (E1000_READ_REG(&sc->hw, E1000_PBA) & 0xffff) );
	device_printf(dev, "Flow control watermarks high = %d low = %d\n",
	    sc->hw.fc.high_water, sc->hw.fc.low_water);
	device_printf(dev, "tx_int_delay = %d, tx_abs_int_delay = %d\n",
	    E1000_READ_REG(&sc->hw, E1000_TIDV),
	    E1000_READ_REG(&sc->hw, E1000_TADV));
	device_printf(dev, "rx_int_delay = %d, rx_abs_int_delay = %d\n",
	    E1000_READ_REG(&sc->hw, E1000_RDTR),
	    E1000_READ_REG(&sc->hw, E1000_RADV));
	device_printf(dev, "hw tdh = %d, hw tdt = %d\n",
	    E1000_READ_REG(&sc->hw, E1000_TDH(0)),
	    E1000_READ_REG(&sc->hw, E1000_TDT(0)));
	device_printf(dev, "hw rdh = %d, hw rdt = %d\n",
	    E1000_READ_REG(&sc->hw, E1000_RDH(0)),
	    E1000_READ_REG(&sc->hw, E1000_RDT(0)));
	device_printf(dev, "Num Tx descriptors avail = %d\n",
	    sc->num_tx_desc_avail);
	device_printf(dev, "Tx Descriptors not avail1 = %ld\n",
	    sc->no_tx_desc_avail1);
	device_printf(dev, "Tx Descriptors not avail2 = %ld\n",
	    sc->no_tx_desc_avail2);
	device_printf(dev, "Std mbuf failed = %ld\n",
	    sc->mbuf_alloc_failed);
	device_printf(dev, "Std mbuf cluster failed = %ld\n",
	    sc->rx_data[0].mbuf_cluster_failed);
	device_printf(dev, "Driver dropped packets = %ld\n",
	    sc->dropped_pkts);
	device_printf(dev, "Driver tx dma failure in encap = %ld\n",
	    sc->no_tx_dma_setup);

	device_printf(dev, "TXCSUM try pullup = %lu\n",
	    sc->tx_csum_try_pullup);
	device_printf(dev, "TXCSUM m_pullup(eh) called = %lu\n",
	    sc->tx_csum_pullup1);
	device_printf(dev, "TXCSUM m_pullup(eh) failed = %lu\n",
	    sc->tx_csum_pullup1_failed);
	device_printf(dev, "TXCSUM m_pullup(eh+ip) called = %lu\n",
	    sc->tx_csum_pullup2);
	device_printf(dev, "TXCSUM m_pullup(eh+ip) failed = %lu\n",
	    sc->tx_csum_pullup2_failed);
	device_printf(dev, "TXCSUM non-writable(eh) droped = %lu\n",
	    sc->tx_csum_drop1);
	device_printf(dev, "TXCSUM non-writable(eh+ip) droped = %lu\n",
	    sc->tx_csum_drop2);
}

static void
emx_print_hw_stats(struct emx_softc *sc)
{
	device_t dev = sc->dev;

	device_printf(dev, "Excessive collisions = %lld\n",
	    (long long)sc->stats.ecol);
#if (DEBUG_HW > 0)  /* Dont output these errors normally */
	device_printf(dev, "Symbol errors = %lld\n",
	    (long long)sc->stats.symerrs);
#endif
	device_printf(dev, "Sequence errors = %lld\n",
	    (long long)sc->stats.sec);
	device_printf(dev, "Defer count = %lld\n",
	    (long long)sc->stats.dc);
	device_printf(dev, "Missed Packets = %lld\n",
	    (long long)sc->stats.mpc);
	device_printf(dev, "Receive No Buffers = %lld\n",
	    (long long)sc->stats.rnbc);
	/* RLEC is inaccurate on some hardware, calculate our own. */
	device_printf(dev, "Receive Length Errors = %lld\n",
	    ((long long)sc->stats.roc + (long long)sc->stats.ruc));
	device_printf(dev, "Receive errors = %lld\n",
	    (long long)sc->stats.rxerrc);
	device_printf(dev, "Crc errors = %lld\n",
	    (long long)sc->stats.crcerrs);
	device_printf(dev, "Alignment errors = %lld\n",
	    (long long)sc->stats.algnerrc);
	device_printf(dev, "Collision/Carrier extension errors = %lld\n",
	    (long long)sc->stats.cexterr);
	device_printf(dev, "RX overruns = %ld\n", sc->rx_overruns);
	device_printf(dev, "watchdog timeouts = %ld\n",
	    sc->watchdog_events);
	device_printf(dev, "XON Rcvd = %lld\n",
	    (long long)sc->stats.xonrxc);
	device_printf(dev, "XON Xmtd = %lld\n",
	    (long long)sc->stats.xontxc);
	device_printf(dev, "XOFF Rcvd = %lld\n",
	    (long long)sc->stats.xoffrxc);
	device_printf(dev, "XOFF Xmtd = %lld\n",
	    (long long)sc->stats.xofftxc);
	device_printf(dev, "Good Packets Rcvd = %lld\n",
	    (long long)sc->stats.gprc);
	device_printf(dev, "Good Packets Xmtd = %lld\n",
	    (long long)sc->stats.gptc);
}

static void
emx_print_nvm_info(struct emx_softc *sc)
{
	uint16_t eeprom_data;
	int i, j, row = 0;

	/* Its a bit crude, but it gets the job done */
	kprintf("\nInterface EEPROM Dump:\n");
	kprintf("Offset\n0x0000  ");
	for (i = 0, j = 0; i < 32; i++, j++) {
		if (j == 8) { /* Make the offset block */
			j = 0; ++row;
			kprintf("\n0x00%x0  ",row);
		}
		e1000_read_nvm(&sc->hw, i, 1, &eeprom_data);
		kprintf("%04x ", eeprom_data);
	}
	kprintf("\n");
}

static int
emx_sysctl_debug_info(SYSCTL_HANDLER_ARGS)
{
	struct emx_softc *sc;
	struct ifnet *ifp;
	int error, result;

	result = -1;
	error = sysctl_handle_int(oidp, &result, 0, req);
	if (error || !req->newptr)
		return (error);

	sc = (struct emx_softc *)arg1;
	ifp = &sc->arpcom.ac_if;

	lwkt_serialize_enter(ifp->if_serializer);

	if (result == 1)
		emx_print_debug_info(sc);

	/*
	 * This value will cause a hex dump of the
	 * first 32 16-bit words of the EEPROM to
	 * the screen.
	 */
	if (result == 2)
		emx_print_nvm_info(sc);

	lwkt_serialize_exit(ifp->if_serializer);

	return (error);
}

static int
emx_sysctl_stats(SYSCTL_HANDLER_ARGS)
{
	int error, result;

	result = -1;
	error = sysctl_handle_int(oidp, &result, 0, req);
	if (error || !req->newptr)
		return (error);

	if (result == 1) {
		struct emx_softc *sc = (struct emx_softc *)arg1;
		struct ifnet *ifp = &sc->arpcom.ac_if;

		lwkt_serialize_enter(ifp->if_serializer);
		emx_print_hw_stats(sc);
		lwkt_serialize_exit(ifp->if_serializer);
	}
	return (error);
}

static void
emx_add_sysctl(struct emx_softc *sc)
{
#ifdef PROFILE_SERIALIZER
	struct ifnet *ifp = &sc->arpcom.ac_if;
#endif
#ifdef EMX_RSS_DEBUG
	char rx_pkt[32];
	int i;
#endif

	sysctl_ctx_init(&sc->sysctl_ctx);
	sc->sysctl_tree = SYSCTL_ADD_NODE(&sc->sysctl_ctx,
				SYSCTL_STATIC_CHILDREN(_hw), OID_AUTO,
				device_get_nameunit(sc->dev),
				CTLFLAG_RD, 0, "");
	if (sc->sysctl_tree == NULL) {
		device_printf(sc->dev, "can't add sysctl node\n");
		return;
	}

	SYSCTL_ADD_PROC(&sc->sysctl_ctx, SYSCTL_CHILDREN(sc->sysctl_tree),
			OID_AUTO, "debug", CTLTYPE_INT|CTLFLAG_RW, sc, 0,
			emx_sysctl_debug_info, "I", "Debug Information");

	SYSCTL_ADD_PROC(&sc->sysctl_ctx, SYSCTL_CHILDREN(sc->sysctl_tree),
			OID_AUTO, "stats", CTLTYPE_INT|CTLFLAG_RW, sc, 0,
			emx_sysctl_stats, "I", "Statistics");

	SYSCTL_ADD_INT(&sc->sysctl_ctx, SYSCTL_CHILDREN(sc->sysctl_tree),
		       OID_AUTO, "rxd", CTLFLAG_RD,
		       &sc->rx_data[0].num_rx_desc, 0, NULL);
	SYSCTL_ADD_INT(&sc->sysctl_ctx, SYSCTL_CHILDREN(sc->sysctl_tree),
		       OID_AUTO, "txd", CTLFLAG_RD, &sc->num_tx_desc, 0, NULL);

#ifdef PROFILE_SERIALIZER
	SYSCTL_ADD_UINT(&sc->sysctl_ctx, SYSCTL_CHILDREN(sc->sysctl_tree),
			OID_AUTO, "serializer_sleep", CTLFLAG_RW,
			&ifp->if_serializer->sleep_cnt, 0, NULL);
	SYSCTL_ADD_UINT(&sc->sysctl_ctx, SYSCTL_CHILDREN(sc->sysctl_tree),
			OID_AUTO, "serializer_tryfail", CTLFLAG_RW,
			&ifp->if_serializer->tryfail_cnt, 0, NULL);
	SYSCTL_ADD_UINT(&sc->sysctl_ctx, SYSCTL_CHILDREN(sc->sysctl_tree),
			OID_AUTO, "serializer_enter", CTLFLAG_RW,
			&ifp->if_serializer->enter_cnt, 0, NULL);
	SYSCTL_ADD_UINT(&sc->sysctl_ctx, SYSCTL_CHILDREN(sc->sysctl_tree),
			OID_AUTO, "serializer_try", CTLFLAG_RW,
			&ifp->if_serializer->try_cnt, 0, NULL);
#endif

	SYSCTL_ADD_PROC(&sc->sysctl_ctx, SYSCTL_CHILDREN(sc->sysctl_tree),
			OID_AUTO, "int_throttle_ceil", CTLTYPE_INT|CTLFLAG_RW,
			sc, 0, emx_sysctl_int_throttle, "I",
			"interrupt throttling rate");
	SYSCTL_ADD_PROC(&sc->sysctl_ctx, SYSCTL_CHILDREN(sc->sysctl_tree),
			OID_AUTO, "int_tx_nsegs", CTLTYPE_INT|CTLFLAG_RW,
			sc, 0, emx_sysctl_int_tx_nsegs, "I",
			"# segments per TX interrupt");

	SYSCTL_ADD_INT(&sc->sysctl_ctx, SYSCTL_CHILDREN(sc->sysctl_tree),
		       OID_AUTO, "rx_ring_inuse", CTLFLAG_RD,
		       &sc->rx_ring_inuse, 0, "RX ring in use");

#ifdef EMX_RSS_DEBUG
	SYSCTL_ADD_INT(&sc->sysctl_ctx, SYSCTL_CHILDREN(sc->sysctl_tree),
		       OID_AUTO, "rss_debug", CTLFLAG_RW, &sc->rss_debug,
		       0, "RSS debug level");
	for (i = 0; i < sc->rx_ring_cnt; ++i) {
		ksnprintf(rx_pkt, sizeof(rx_pkt), "rx%d_pkt", i);
		SYSCTL_ADD_UINT(&sc->sysctl_ctx,
				SYSCTL_CHILDREN(sc->sysctl_tree), OID_AUTO,
				rx_pkt, CTLFLAG_RD,
				&sc->rx_data[i].rx_pkts, 0, "RXed packets");
	}
#endif
}

static int
emx_sysctl_int_throttle(SYSCTL_HANDLER_ARGS)
{
	struct emx_softc *sc = (void *)arg1;
	struct ifnet *ifp = &sc->arpcom.ac_if;
	int error, throttle;

	throttle = sc->int_throttle_ceil;
	error = sysctl_handle_int(oidp, &throttle, 0, req);
	if (error || req->newptr == NULL)
		return error;
	if (throttle < 0 || throttle > 1000000000 / 256)
		return EINVAL;

	if (throttle) {
		/*
		 * Set the interrupt throttling rate in 256ns increments,
		 * recalculate sysctl value assignment to get exact frequency.
		 */
		throttle = 1000000000 / 256 / throttle;

		/* Upper 16bits of ITR is reserved and should be zero */
		if (throttle & 0xffff0000)
			return EINVAL;
	}

	lwkt_serialize_enter(ifp->if_serializer);

	if (throttle)
		sc->int_throttle_ceil = 1000000000 / 256 / throttle;
	else
		sc->int_throttle_ceil = 0;

	if (ifp->if_flags & IFF_RUNNING)
		E1000_WRITE_REG(&sc->hw, E1000_ITR, throttle);

	lwkt_serialize_exit(ifp->if_serializer);

	if (bootverbose) {
		if_printf(ifp, "Interrupt moderation set to %d/sec\n",
			  sc->int_throttle_ceil);
	}
	return 0;
}

static int
emx_sysctl_int_tx_nsegs(SYSCTL_HANDLER_ARGS)
{
	struct emx_softc *sc = (void *)arg1;
	struct ifnet *ifp = &sc->arpcom.ac_if;
	int error, segs;

	segs = sc->tx_int_nsegs;
	error = sysctl_handle_int(oidp, &segs, 0, req);
	if (error || req->newptr == NULL)
		return error;
	if (segs <= 0)
		return EINVAL;

	lwkt_serialize_enter(ifp->if_serializer);

	/*
	 * Don't allow int_tx_nsegs to become:
	 * o  Less the oact_tx_desc
	 * o  Too large that no TX desc will cause TX interrupt to
	 *    be generated (OACTIVE will never recover)
	 * o  Too small that will cause tx_dd[] overflow
	 */
	if (segs < sc->oact_tx_desc ||
	    segs >= sc->num_tx_desc - sc->oact_tx_desc ||
	    segs < sc->num_tx_desc / EMX_TXDD_SAFE) {
		error = EINVAL;
	} else {
		error = 0;
		sc->tx_int_nsegs = segs;
	}

	lwkt_serialize_exit(ifp->if_serializer);

	return error;
}

static int
emx_dma_alloc(struct emx_softc *sc)
{
	int error, i;

	/*
	 * Create top level busdma tag
	 */
	error = bus_dma_tag_create(NULL, 1, 0,
			BUS_SPACE_MAXADDR, BUS_SPACE_MAXADDR,
			NULL, NULL,
			BUS_SPACE_MAXSIZE_32BIT, 0, BUS_SPACE_MAXSIZE_32BIT,
			0, &sc->parent_dtag);
	if (error) {
		device_printf(sc->dev, "could not create top level DMA tag\n");
		return error;
	}

	/*
	 * Allocate transmit descriptors ring and buffers
	 */
	error = emx_create_tx_ring(sc);
	if (error) {
		device_printf(sc->dev, "Could not setup transmit structures\n");
		return error;
	}

	/*
	 * Allocate receive descriptors ring and buffers
	 */
	for (i = 0; i < sc->rx_ring_cnt; ++i) {
		error = emx_create_rx_ring(sc, &sc->rx_data[i]);
		if (error) {
			device_printf(sc->dev,
			    "Could not setup receive structures\n");
			return error;
		}
	}
	return 0;
}

static void
emx_dma_free(struct emx_softc *sc)
{
	int i;

	emx_destroy_tx_ring(sc, sc->num_tx_desc);

	for (i = 0; i < sc->rx_ring_cnt; ++i) {
		emx_destroy_rx_ring(sc, &sc->rx_data[i],
				    sc->rx_data[i].num_rx_desc);
	}

	/* Free top level busdma tag */
	if (sc->parent_dtag != NULL)
		bus_dma_tag_destroy(sc->parent_dtag);
}
