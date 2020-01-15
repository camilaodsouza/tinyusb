/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2019 Peter Lawrence
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * This file is part of the TinyUSB stack.
 */

/*
  Theory of operation:

  The NUC121/NUC125/NUC126 USBD peripheral has eight "EP"s, but each is simplex, 
  so two collectively (peripheral nomenclature of "EP0" and "EP1") are needed to 
  implement USB EP0.  PERIPH_EP0 and PERIPH_EP1 are used by this driver for 
  EP0_IN and EP0_OUT respectively.  This leaves up to six for user usage.
*/

#include "tusb_option.h"

#if TUSB_OPT_DEVICE_ENABLED && ( (CFG_TUSB_MCU == OPT_MCU_NUC121) || (CFG_TUSB_MCU == OPT_MCU_NUC126) )

#include "device/dcd.h"
#include "NuMicro.h"

/* allocation of USBD RAM for Setup, EP0_IN, and and EP_OUT */
#define PERIPH_SETUP_BUF_BASE  0
#define PERIPH_SETUP_BUF_LEN   8
#define PERIPH_EP0_BUF_BASE    (PERIPH_SETUP_BUF_BASE + PERIPH_SETUP_BUF_LEN)
#define PERIPH_EP0_BUF_LEN     CFG_TUD_ENDPOINT0_SIZE
#define PERIPH_EP1_BUF_BASE    (PERIPH_EP0_BUF_BASE + PERIPH_EP0_BUF_LEN)
#define PERIPH_EP1_BUF_LEN     CFG_TUD_ENDPOINT0_SIZE
#define PERIPH_EP2_BUF_BASE    (PERIPH_EP1_BUF_BASE + PERIPH_EP1_BUF_LEN)

/* rather important info unfortunately not provided by device include files: how much there is */
#define USBD_BUF_SIZE          ((CFG_TUSB_MCU == OPT_MCU_NUC121) ? 768 : 512)

enum ep_enum
{
  PERIPH_EP0 = 0,
  PERIPH_EP1 = 1,
  PERIPH_EP2 = 2,
  PERIPH_EP3 = 3,
  PERIPH_EP4 = 4,
  PERIPH_EP5 = 5,
  PERIPH_EP6 = 6,
  PERIPH_EP7 = 7,
  PERIPH_MAX_EP,
};

/* set by dcd_set_address() */
static volatile uint8_t assigned_address;

/* reset by dcd_init(), this is used by dcd_edpt_open() to assign USBD peripheral buffer addresses */
static uint32_t bufseg_addr;

/* used by dcd_edpt_xfer() and the ISR to reset the data sync (DATA0/DATA1) in an EP0_IN transfer */
static bool active_ep0_xfer;

/* RAM table needed to track ongoing transfers performed by dcd_edpt_xfer(), dcd_in_xfer(), and the ISR */
static struct xfer_ctl_t
{
  uint8_t *data_ptr;         /* collectively, data_ptr and remaining_bytes track progress of endpoint transfers */
  uint16_t remaining_bytes;
  uint16_t max_packet_size;  /* needed since device driver only finds out this at runtime */
  uint16_t total_bytes;      /* quantity needed to pass as argument to dcd_event_xfer_complete() (for IN endpoints) */
} xfer_table[PERIPH_MAX_EP];

/*
  local helper functions
*/

static void usb_attach(void)
{
  USBD->SE0 &= ~USBD_SE0_SE0_Msk;
}

static void usb_detach(void)
{
  USBD->SE0 |= USBD_SE0_SE0_Msk;
}

static void usb_control_send_zlp(void)
{
  USBD->EP[PERIPH_EP0].CFG |= USBD_CFG_DSQSYNC_Msk;
  USBD->EP[PERIPH_EP0].MXPLD = 0;
}

/* reconstruct ep_addr from particular USB Configuration Register */
static uint8_t decode_ep_addr(USBD_EP_T *ep)
{
  uint8_t ep_addr = ep->CFG & USBD_CFG_EPNUM_Msk;
  if ( USBD_CFG_EPMODE_IN == (ep->CFG & USBD_CFG_STATE_Msk) )
    ep_addr |= TUSB_DIR_IN_MASK;
  return ep_addr;
}

/* map 8-bit ep_addr into peripheral endpoint index (PERIPH_EP0...) */
static USBD_EP_T *ep_entry(uint8_t ep_addr, bool add)
{
  USBD_EP_T *ep;
  enum ep_enum ep_index;

  for (ep_index = PERIPH_EP0, ep = USBD->EP; ep_index < PERIPH_MAX_EP; ep_index++, ep++)
  {
    if (add)
    {
      /* take first peripheral endpoint that is unused */
      if (0 == (ep->CFG & USBD_CFG_STATE_Msk)) return ep;
    }
    else
    {
      /* find a peripheral endpoint that matches ep_addr */
      uint8_t candidate_ep_addr = decode_ep_addr(ep);
      if (candidate_ep_addr == ep_addr) return ep;
    }
  }

  return NULL;
}

/* perform an IN endpoint transfer; this is called by dcd_edpt_xfer() and the ISR  */
static void dcd_in_xfer(struct xfer_ctl_t *xfer, USBD_EP_T *ep)
{
  uint16_t bytes_now = tu_min16(xfer->remaining_bytes, xfer->max_packet_size);

  memcpy((uint8_t *)(USBD_BUF_BASE + ep->BUFSEG), xfer->data_ptr, bytes_now);
  ep->MXPLD = bytes_now;
}

/* centralized location for USBD interrupt enable bit mask */
static const uint32_t enabled_irqs = USBD_INTSTS_VBDETIF_Msk | USBD_INTSTS_BUSIF_Msk | USBD_INTSTS_SETUP_Msk | USBD_INTSTS_USBIF_Msk | USBD_INTSTS_SOFIF_Msk;

/*
  NUC121/NUC125/NUC126 TinyUSB API driver implementation
*/

void dcd_init(uint8_t rhport)
{
  (void) rhport;

#ifdef SUPPORT_LPM
  USBD->ATTR = 0x7D0 | USBD_LPMACK;
#else
  USBD->ATTR = 0x7D0;
#endif

  usb_detach();

  USBD->STBUFSEG = PERIPH_SETUP_BUF_BASE;

  for (enum ep_enum ep_index = PERIPH_EP0; ep_index < PERIPH_MAX_EP; ep_index++)
  {
    USBD->EP[ep_index].CFGP &= ~USBD_CFG_STATE_Msk;
  }

  /* allocate the default EP0 endpoints */

  USBD->EP[PERIPH_EP0].CFG = USBD_CFG_CSTALL_Msk | USBD_CFG_EPMODE_IN;
  USBD->EP[PERIPH_EP0].BUFSEG = PERIPH_EP0_BUF_BASE;
  xfer_table[PERIPH_EP0].max_packet_size = PERIPH_EP0_BUF_LEN;

  USBD->EP[PERIPH_EP1].CFG = USBD_CFG_CSTALL_Msk | USBD_CFG_EPMODE_OUT;
  USBD->EP[PERIPH_EP1].BUFSEG = PERIPH_EP1_BUF_BASE;
  xfer_table[PERIPH_EP1].max_packet_size = PERIPH_EP1_BUF_LEN;

  /* USB RAM beyond what we've allocated above is available to the user */
  bufseg_addr = PERIPH_EP2_BUF_BASE;

  usb_attach();

  USBD->INTSTS = enabled_irqs;
  USBD->INTEN  = enabled_irqs;
}

void dcd_int_enable(uint8_t rhport)
{
  (void) rhport;
  NVIC_EnableIRQ(USBD_IRQn);
}

void dcd_int_disable(uint8_t rhport)
{
  (void) rhport;
  NVIC_DisableIRQ(USBD_IRQn);
}

void dcd_set_address(uint8_t rhport, uint8_t dev_addr)
{
  (void) rhport;
  usb_control_send_zlp(); /* SET_ADDRESS is the one exception where TinyUSB doesn't use dcd_edpt_xfer() to generate a ZLP */
  assigned_address = dev_addr;
}

void dcd_set_config(uint8_t rhport, uint8_t config_num)
{
  (void) rhport;
  (void) config_num;
}

void dcd_remote_wakeup(uint8_t rhport)
{
  (void) rhport;
  USBD->ATTR = USBD_ATTR_RWAKEUP_Msk;
}

bool dcd_edpt_open(uint8_t rhport, tusb_desc_endpoint_t const * p_endpoint_desc)
{
  (void) rhport;

  USBD_EP_T *ep = ep_entry(p_endpoint_desc->bEndpointAddress, true);
  TU_ASSERT(ep);

  /* mine the data for the information we need */
  int const dir = tu_edpt_dir(p_endpoint_desc->bEndpointAddress);
  int const size = p_endpoint_desc->wMaxPacketSize.size;
  tusb_xfer_type_t const type = p_endpoint_desc->bmAttributes.xfer;
  struct xfer_ctl_t *xfer = &xfer_table[ep - USBD->EP];

  /* allocate buffer from USB RAM */
  ep->BUFSEG = bufseg_addr;
  bufseg_addr += size;
  TU_ASSERT(bufseg_addr <= USBD_BUF_SIZE);

  /* construct USB Configuration Register value and then write it */
  uint32_t cfg = tu_edpt_number(p_endpoint_desc->bEndpointAddress);
  cfg |= (TUSB_DIR_IN == dir) ? USBD_CFG_EPMODE_IN : USBD_CFG_EPMODE_OUT;
  if (TUSB_XFER_ISOCHRONOUS == type)
    cfg |= USBD_CFG_TYPE_ISO;
  ep->CFG = cfg;

  /* make a note of the endpoint size */
  xfer->max_packet_size = size;

  return true;
}

bool dcd_edpt_xfer(uint8_t rhport, uint8_t ep_addr, uint8_t *buffer, uint16_t total_bytes)
{
  (void) rhport;

  /* mine the data for the information we need */
  tusb_dir_t dir = tu_edpt_dir(ep_addr);
  USBD_EP_T *ep = ep_entry(ep_addr, false);
  struct xfer_ctl_t *xfer = &xfer_table[ep - USBD->EP];

  /* store away the information we'll needing now and later */
  xfer->data_ptr = buffer;
  xfer->remaining_bytes = total_bytes;
  xfer->total_bytes = total_bytes;

  /* for the first of one or more EP0_IN packets in a message, the first must be DATA1 */
  if ( (0x80 == ep_addr) && !active_ep0_xfer ) ep->CFG |= USBD_CFG_DSQSYNC_Msk;

  if (TUSB_DIR_IN == dir)
    dcd_in_xfer(xfer, ep);
  else
    ep->MXPLD = xfer->max_packet_size;

  return true;
}

void dcd_edpt_stall(uint8_t rhport, uint8_t ep_addr)
{
  (void) rhport;
  USBD_EP_T *ep = ep_entry(ep_addr, false);
  ep->CFGP |= USBD_CFGP_SSTALL_Msk;
}

void dcd_edpt_clear_stall(uint8_t rhport, uint8_t ep_addr)
{
  (void) rhport;
  USBD_EP_T *ep = ep_entry(ep_addr, false);
  ep->CFG |= USBD_CFG_CSTALL_Msk;
}

void USBD_IRQHandler(void)
{
  uint32_t status = USBD->INTSTS;
#ifdef SUPPORT_LPM
  uint32_t state = USBD->ATTR & 0x300f;
#else
  uint32_t state = USBD->ATTR & 0xf;
#endif

  if(status & USBD_INTSTS_VBDETIF_Msk)
  {
    if(USBD->VBUSDET & USBD_VBUSDET_VBUSDET_Msk)
    {
      /* USB connect */
      USBD->ATTR |= USBD_ATTR_USBEN_Msk | USBD_ATTR_PHYEN_Msk;
    }
    else
    {
      /* USB disconnect */
      USBD->ATTR &= ~USBD_ATTR_USBEN_Msk;
    }
  }

  if(status & USBD_INTSTS_BUSIF_Msk)
  {
    if(state & USBD_ATTR_USBRST_Msk)
    {
      /* USB bus reset */
      USBD->ATTR |= USBD_ATTR_USBEN_Msk | USBD_ATTR_PHYEN_Msk;

      /* Reset all endpoints to DATA0 */
      for(enum ep_enum ep_index = PERIPH_EP0; ep_index < PERIPH_MAX_EP; ep_index++)
        USBD->EP[ep_index].CFG &= ~USBD_CFG_DSQSYNC_Msk;

      /* Reset USB device address */
      USBD->FADDR = 0;

      /* reset EP0_IN flag */
      active_ep0_xfer = false;

      dcd_event_bus_signal(0, DCD_EVENT_BUS_RESET, true);
    }

    if(state & USBD_ATTR_SUSPEND_Msk)
    {
      /* Enable USB but disable PHY */
      USBD->ATTR &= ~USBD_ATTR_PHYEN_Msk;
      dcd_event_bus_signal(0, DCD_EVENT_SUSPEND, true);
    }

    if(state & USBD_ATTR_RESUME_Msk)
    {
      /* Enable USB and enable PHY */
      USBD->ATTR |= USBD_ATTR_USBEN_Msk | USBD_ATTR_PHYEN_Msk;
      dcd_event_bus_signal(0, DCD_EVENT_RESUME, true);
    }
  }

  if(status & USBD_INTSTS_SETUP_Msk)
  {
    /* clear the data ready flag of control endpoints */
    USBD->EP[PERIPH_EP0].CFGP |= USBD_CFGP_CLRRDY_Msk;
    USBD->EP[PERIPH_EP1].CFGP |= USBD_CFGP_CLRRDY_Msk;

    /* get SETUP packet from USB buffer */
    dcd_event_setup_received(0, (uint8_t *)USBD_BUF_BASE, true);
  }

  if(status & USBD_INTSTS_USBIF_Msk)
  {
    if (status & USBD_INTSTS_EPEVT0_Msk) /* PERIPH_EP0 (EP0_IN) event: this is treated separately from the rest */
    {
      /* given ACK from host has happened, we can now set the address (if not already done) */
      if((USBD->FADDR != assigned_address) && (USBD->FADDR == 0)) USBD->FADDR = assigned_address;

      uint16_t const available_bytes = USBD->EP[PERIPH_EP0].MXPLD;

      active_ep0_xfer = (available_bytes == xfer_table[PERIPH_EP0].max_packet_size);

      dcd_event_xfer_complete(0, 0x80, available_bytes, XFER_RESULT_SUCCESS, true);
    }

    /* service PERIPH_EP1 through PERIPH_EP7 */
    enum ep_enum ep_index;
    uint32_t mask;
    struct xfer_ctl_t *xfer;
    USBD_EP_T *ep;
    for (ep_index = PERIPH_EP1, mask = USBD_INTSTS_EPEVT1_Msk, xfer = &xfer_table[PERIPH_EP1], ep = &USBD->EP[PERIPH_EP1]; ep_index <= PERIPH_EP7; ep_index++, mask <<= 1, xfer++, ep++)
    {
      if(status & mask)
      {
        USBD->INTSTS = mask;

        uint16_t const available_bytes = ep->MXPLD;
        uint8_t const ep_addr = decode_ep_addr(ep);
        bool const out_ep = !(ep_addr & TUSB_DIR_IN_MASK);

        if (out_ep)
        {
          /* copy the data from the PC to the previously provided buffer */
          memcpy(xfer->data_ptr, (uint8_t *)(USBD_BUF_BASE + ep->BUFSEG), available_bytes);
          xfer->remaining_bytes -= available_bytes;
          xfer->data_ptr += available_bytes;

          /* when the transfer is finished, alert TinyUSB; otherwise, accept more data */
          if ( (0 == xfer->remaining_bytes) || (available_bytes < xfer->max_packet_size) )
            dcd_event_xfer_complete(0, ep_addr, xfer->total_bytes, XFER_RESULT_SUCCESS, true);
          else if (xfer->remaining_bytes)
            ep->MXPLD = xfer->max_packet_size;
        }
        else
        {
          /* update the bookkeeping to reflect the data that has now been sent to the PC */
          xfer->remaining_bytes -= available_bytes;
          xfer->data_ptr += available_bytes;

          /* if more data to send, send it; otherwise, alert TinyUSB that we've finished */
          if (xfer->remaining_bytes)
            dcd_in_xfer(xfer, ep);
          else
            dcd_event_xfer_complete(0, ep_addr, xfer->total_bytes, XFER_RESULT_SUCCESS, true);
        }
      }
    }
  }

  if(status & USBD_INTSTS_SOFIF_Msk)
  {
    /* Start-Of-Frame event */
    dcd_event_bus_signal(0, DCD_EVENT_SOF, true);
  }

  /* acknowledge all interrupts */
  USBD->INTSTS = status & enabled_irqs;
}

void dcd_isr(uint8_t rhport)
{
  (void) rhport;
  USBD_IRQHandler();
}

#endif