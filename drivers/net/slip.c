/****************************************************************************
 * drivers/net/slip.c
 *
 *   Copyright (C) 2011-2012, 2015-2018 Gregory Nutt. All rights reserved.
 *   Author: Gregory Nutt <gnutt@nuttx.org>
 *
 * Reference: RFC 1055
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
 * 3. Neither the name NuttX nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/types.h>
#include <sys/stat.h>

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <time.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <debug.h>

#include <nuttx/irq.h>
#include <nuttx/clock.h>
#include <nuttx/signal.h>
#include <nuttx/kthread.h>
#include <nuttx/semaphore.h>
#include <nuttx/net/net.h>
#include <nuttx/net/netdev.h>
#include <nuttx/net/ip.h>
#include <nuttx/net/slip.h>

#if defined(CONFIG_NET) && defined(CONFIG_NET_SLIP)

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* NOTE:  Slip requires UART hardware handshake.  If hardware handshake is
 * not available with your UART, then you might try the 'slattach' option
 * -L which enable "3-wire operation."  That allows operation without the
 * hardware handshake (but with the possibility of data overrun).
 */

/* Configuration ************************************************************/

#ifndef CONFIG_NET_SLIP_STACKSIZE
#  define CONFIG_NET_SLIP_STACKSIZE 2048
#endif

#ifndef CONFIG_NET_SLIP_DEFPRIO
#  define CONFIG_NET_SLIP_DEFPRIO 128
#endif

/* The Linux slip module hard-codes its MTU size to 296 (40 bytes for the
 * IP+TPC headers plus 256 bytes of data).  So you might as well set
 * CONFIG_NET_SLIP_PKTSIZE to 296 as well.
 *
 * There may be an issue with this setting, however.  I see that Linux uses
 * a MTU of 296 and window of 256, but actually only sends 168 bytes of data:
 * 40 + 128.  I believe that is to allow for the 2x worst cast packet
 * expansion.  Ideally we would like to advertise the 256 MSS, but restrict
 * transfers to 128 bytes (possibly by modifying the tcp_mss() macro).
 */

#if CONFIG_NET_SLIP_PKTSIZE < 296
#  error "CONFIG_NET_SLIP_PKTSIZE >= 296 is required"
#endif

/* CONFIG_NET_SLIP_NINTERFACES determines the number of physical interfaces
 * that will be supported.
 */

#ifndef CONFIG_NET_SLIP_NINTERFACES
# define CONFIG_NET_SLIP_NINTERFACES 1
#endif

/*  SLIP special character codes *******************************************/

#define SLIP_END      0300    /* Indicates end of packet */
#define SLIP_ESC      0333    /* Indicates byte stuffing */
#define SLIP_ESC_END  0334    /* ESC ESC_END means SLIP_END data byte */
#define SLIP_ESC_ESC  0335    /* ESC ESC_ESC means ESC data byte */

/* General driver definitions **********************************************/

/* TX poll delay = 1 second = 1000000 microseconds. */

#define SLIP_WDDELAY   (1*1000000)

/* This is a helper pointer for accessing the contents of the ip header */

#define IPv4BUF        ((FAR struct ipv4_hdr_s *)priv->dev.d_buf)
#define IPv6BUF        ((FAR struct ipv6_hdr_s *)priv->dev.d_buf)

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* The slip_driver_s encapsulates all state information for a single hardware
 * interface
 */

struct slip_driver_s
{
  volatile bool bifup;      /* true:ifup false:ifdown */
  bool          txnodelay;  /* True: nxsig_usleep() not needed */
  int16_t       fd;         /* TTY file descriptor */
  uint16_t      rxlen;      /* The number of bytes in rxbuf */
  pid_t         rxpid;      /* Receiver thread ID */
  pid_t         txpid;      /* Transmitter thread ID */
  sem_t         waitsem;    /* Mutually exclusive access to the network */

  /* This holds the information visible to the NuttX network */

  struct net_driver_s dev;  /* Interface understood by the network */
  uint8_t rxbuf[CONFIG_NET_SLIP_PKTSIZE + 2];
  uint8_t txbuf[CONFIG_NET_SLIP_PKTSIZE + 2];
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* We really should get rid of CONFIG_NET_SLIP_NINTERFACES and, instead,
 * kmm_malloc() new interface instances as needed.
 */

static struct slip_driver_s g_slip[CONFIG_NET_SLIP_NINTERFACES];

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void slip_semtake(FAR struct slip_driver_s *priv);

/* Common TX logic */

static void slip_write(FAR struct slip_driver_s *priv,
                       FAR const uint8_t *buffer, int len);
static void slip_putc(FAR struct slip_driver_s *priv, int ch);
static int slip_transmit(FAR struct slip_driver_s *priv);
static int slip_txpoll(FAR struct net_driver_s *dev);
static void slip_txtask(int argc, FAR char *argv[]);

/* Packet receiver task */

static int slip_getc(FAR struct slip_driver_s *priv);
static inline void slip_receive(FAR struct slip_driver_s *priv);
static int slip_rxtask(int argc, FAR char *argv[]);

/* NuttX callback functions */

static int slip_ifup(FAR struct net_driver_s *dev);
static int slip_ifdown(FAR struct net_driver_s *dev);
static int slip_txavail(FAR struct net_driver_s *dev);
#ifdef CONFIG_NET_MCASTGROUP
static int slip_addmac(FAR struct net_driver_s *dev, FAR const uint8_t *mac);
static int slip_rmmac(FAR struct net_driver_s *dev, FAR const uint8_t *mac);
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: slip_semtake
 ****************************************************************************/

static void slip_semtake(FAR struct slip_driver_s *priv)
{
  int ret;

  do
    {
      /* Take the semaphore (perhaps waiting) */

      ret = nxsem_wait(&priv->waitsem);

      /* The only case that an error should occur here is if the wait was
       * awakened by a signal.
       */

      DEBUGASSERT(ret == OK || ret == -EINTR);
    }
  while (ret == -EINTR);
}

#define slip_semgive(p) nxsem_post(&(p)->waitsem);

/****************************************************************************
 * Name: slip_write
 *
 * Description:
 *   Just an inline wrapper around fwrite with error checking.
 *
 * Input Parameters:
 *   priv - Reference to the driver state structure
 *   buffer - Buffer data to send
 *   len - Buffer length in bytes
 *
 ****************************************************************************/

static inline void slip_write(FAR struct slip_driver_s *priv,
                              FAR const uint8_t *buffer, int len)
{
  /* Handle the case where the write is awakened by a signal */

  while (write(priv->fd, buffer, len) < 0)
    {
      DEBUGASSERT(errno == EINTR);
    }
}

/****************************************************************************
 * Name: slip_putc
 *
 * Description:
 *   Just an inline wrapper around putc with error checking.
 *
 * Input Parameters:
 *   priv - Reference to the driver state structure
 *   ch - The character to send
 *
 ****************************************************************************/

static inline void slip_putc(FAR struct slip_driver_s *priv, int ch)
{
  uint8_t buffer = (uint8_t)ch;
  slip_write(priv, &buffer, 1);
}

/****************************************************************************
 * Name: slip_transmit
 *
 * Description:
 *   Start hardware transmission.  Called either from the txdone interrupt
 *   handling or from watchdog based polling.
 *
 * Input Parameters:
 *   priv  - Reference to the driver state structure
 *
 * Returned Value:
 *   OK on success; a negated errno on failure
 *
 ****************************************************************************/

static int slip_transmit(FAR struct slip_driver_s *priv)
{
  uint8_t *src;
  uint8_t *start;
  uint8_t  esc;
  int      remaining;
  int      len;

  /* Increment statistics */

  ninfo("Sending packet size %d\n", priv->dev.d_len);
  NETDEV_TXPACKETS(&priv->dev);

  /* Send an initial END character to flush out any data that may have
   * accumulated in the receiver due to line noise
   */

  slip_putc(priv, SLIP_END);

  /* For each byte in the packet, send the appropriate character sequence */

  src       = priv->dev.d_buf;
  remaining = priv->dev.d_len;
  start     = src;
  len       = 0;

  while (remaining-- > 0)
    {
      switch (*src)
        {
          /* If it's the same code as an END character, we send a special two
           * character code so as not to make the receiver think we sent an
           * END
           */

          case SLIP_END:
            esc = SLIP_ESC_END;
            goto escape;

          /* If it's the same code as an ESC character, we send a special two
           * character code so as not to make the receiver think we sent an
           * ESC
           */

          case SLIP_ESC:
            esc = SLIP_ESC_ESC;

          escape:
            {
              /* Flush any unsent data */

              if (len > 0)
                {
                  slip_write(priv, start, len);
                }

              /* Reset */

              start = src + 1;
              len   = 0;

              /* Then send the escape sequence */

              slip_putc(priv, SLIP_ESC);
              slip_putc(priv, esc);
            }
          break;

          /* otherwise, just bump up the count */

          default:
            len++;
            break;
        }

      /* Point to the next character in the packet */

      src++;
    }

  /* We have looked at every character in the packet.  Now flush any unsent
   * data
   */

  if (len > 0)
    {
      slip_write(priv, start, len);
    }

  /* And send the END token */

  slip_putc(priv, SLIP_END);
  NETDEV_TXDONE(&priv->dev);
  priv->txnodelay = true;
  return OK;
}

/****************************************************************************
 * Name: slip_txpoll
 *
 * Description:
 *   Check if the network has any outgoing packets ready to send.  This is a
 *   callback from devif_poll().  devif_poll() may be called:
 *
 *   1. When the preceding TX packet send is complete, or
 *   2. During normal periodic polling
 *
 * Input Parameters:
 *   dev  - Reference to the NuttX driver state structure
 *
 * Returned Value:
 *   OK on success; a negated errno on failure
 *
 * Assumptions:
 *   The initiator of the poll holds the priv->waitsem;
 *
 ****************************************************************************/

static int slip_txpoll(FAR struct net_driver_s *dev)
{
  FAR struct slip_driver_s *priv = (FAR struct slip_driver_s *)dev->d_private;

  /* If the polling resulted in data that should be sent out on the network,
   * the field d_len is set to a value > 0.
   */

  if (priv->dev.d_len > 0)
    {
      if (!devif_loopback(&priv->dev))
        {
          slip_transmit(priv);
        }
    }

  /* If zero is returned, the polling will continue until all connections have
   * been examined.
   */

  return 0;
}

/****************************************************************************
 * Name: slip_txtask
 *
 * Description:
 *   Polling and transmission is performed on tx thread.
 *
 * Input Parameters:
 *   arg  - Reference to the NuttX driver state structure
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

static void slip_txtask(int argc, FAR char *argv[])
{
  FAR struct slip_driver_s *priv;
  unsigned int index = *(argv[1]) - '0';
  clock_t start_ticks;
  clock_t now_ticks;
  unsigned int hsec;

  nerr("index: %d\n", index);
  DEBUGASSERT(index < CONFIG_NET_SLIP_NINTERFACES);

  /* Get our private data structure instance and wake up the waiting
   * initialization logic.
   */

  priv = &g_slip[index];
  slip_semgive(priv);

  /* Loop forever */

  start_ticks = clock_systimer();
  for (;  ; )
    {
      /* Wait for the timeout to expire (or until we are signaled by by  */

      slip_semtake(priv);
      if (!priv->txnodelay)
        {
          slip_semgive(priv);
          nxsig_usleep(SLIP_WDDELAY);
        }
      else
        {
          priv->txnodelay = false;
          slip_semgive(priv);
        }

      /* Is the interface up? */

      if (priv->bifup)
        {
          /* Get exclusive access to the network (if it it is already being
           * used slip_rxtask, then we have to wait).
           */

          slip_semtake(priv);

          /* Poll the networking layer for new XMIT data. */

          net_lock();
          priv->dev.d_buf = priv->txbuf;

          /* Has a half second elapsed since the last timer poll? */

          now_ticks = clock_systimer();
          hsec = (unsigned int)((now_ticks - start_ticks) / TICK_PER_HSEC);
          if (hsec > 0)
            {
              /* Yes, perform the timer poll */

              (void)devif_timer(&priv->dev, slip_txpoll);
              start_ticks += hsec * TICK_PER_HSEC;
            }
          else
            {
              /* No, perform the normal TX poll */

              (void)devif_poll(&priv->dev, slip_txpoll);
            }

          net_unlock();
          slip_semgive(priv);
        }
    }
}

/****************************************************************************
 * Name: slip_getc
 *
 * Description:
 *   Get one byte from the serial input.
 *
 * Input Parameters:
 *   priv - Reference to the driver state structure
 *
 * Returned Value:
 *   The returned byte
 *
 ****************************************************************************/

static inline int slip_getc(FAR struct slip_driver_s *priv)
{
  uint8_t ch;

  while (read(priv->fd, &ch, 1) < 0)
    {
      DEBUGASSERT(errno == EINTR);
    }

  return (int)ch;
}

/****************************************************************************
 * Name: slip_receive
 *
 * Description:
 *   Read a packet from the serial input
 *
 * Input Parameters:
 *   priv  - Reference to the driver state structure
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

static inline void slip_receive(FAR struct slip_driver_s *priv)
{
  uint8_t ch;

  /* Copy the data from the hardware to the RX buffer until we put
   * together a whole packet. Make sure not to copy them into the
   * packet if we run out of room.
   */

  ninfo("Receiving packet\n");
  for (; ; )
    {
      /* Get the next character in the stream. */

      ch = slip_getc(priv);

      /* Handle bytestuffing if necessary */

      switch (ch)
        {
        /* If it's an END character then we're done with the packet.
         * (OR we are just starting a packet)
         */

        case SLIP_END:
          {
            ninfo("END\n");

            /* A minor optimization: if there is no data in the packet,
             * ignore it. This is meant to avoid bothering IP with all the
             * empty packets generated by the duplicate END characters which
             * are in turn sent to try to detect line noise.
             */

            if (priv->rxlen > 0)
              {
                ninfo("Received packet size %d\n", priv->rxlen);
                return;
              }
          }
          break;

        /* if it's the same code as an ESC character, wait and get another
         * character and then figure out what to store in the packet based
         * on that.
         */

        case SLIP_ESC:
          {
            ninfo("ESC\n");
            ch = slip_getc(priv);

            /* if "ch" is not one of these two, then we have a protocol
             * violation.  The best bet seems to be to leave the byte alone
             * and just stuff it into the packet
             */

            switch (ch)
              {
              case SLIP_ESC_END:
                ninfo("ESC-END\n");
                ch = SLIP_END;
                break;

               case SLIP_ESC_ESC:
                ninfo("ESC-ESC\n");
                ch = SLIP_ESC;
                break;

              default:
                nerr("ERROR: Protocol violation: %02x\n", ch);
                break;
              }

            /* Here we fall into the default handler and let it store the
             * character for us
             */
          }

        default:
          {
            if (priv->rxlen < CONFIG_NET_SLIP_PKTSIZE + 2)
              {
                priv->rxbuf[priv->rxlen++] = ch;
              }
          }
          break;
        }
    }
}

/****************************************************************************
 * Name: slip_rxtask
 *
 * Description:
 *   Wait for incoming data.
 *
 * Input Parameters:
 *   argc
 *   argv
 *
 * Returned Value:
 *   (Does not return)
 *
 * Assumptions:
 *
 ****************************************************************************/

static int slip_rxtask(int argc, FAR char *argv[])
{
  FAR struct slip_driver_s *priv;
  unsigned int index = *(argv[1]) - '0';
  int ch;

  nerr("index: %d\n", index);
  DEBUGASSERT(index < CONFIG_NET_SLIP_NINTERFACES);

  /* Get our private data structure instance and wake up the waiting
   * initialization logic.
   */

  priv = &g_slip[index];
  slip_semgive(priv);

  /* Loop forever */

  for (; ; )
    {
      /* Wait for the next character to be available on the input stream. */

      ninfo("Waiting...\n");
      ch = slip_getc(priv);

      /* Ignore any input that we receive before the interface is up. */

      if (!priv->bifup)
        {
          continue;
        }

      /* We have something...
       *
       * END characters may appear at packet boundaries BEFORE as well as
       * after the beginning of the packet.  This is normal and expected.
       */

      if (ch == SLIP_END)
        {
          priv->rxlen = 0;
        }

      /* Otherwise, we are in danger of being out-of-sync.  Apparently the
       * leading END character is optional.  Let's try to continue.
       */

      else
        {
          priv->rxbuf[0] = (uint8_t)ch;
          priv->rxlen    = 1;
        }

      /* Copy the data data from the hardware to priv->rxbuf until we put
       * together a whole packet.
       */

      slip_receive(priv);

      /* Handle the IP input.  Get exclusive access to the network. */

      slip_semtake(priv);
      priv->dev.d_buf = priv->rxbuf;
      priv->dev.d_len = priv->rxlen;

      net_lock();
      NETDEV_RXPACKETS(&priv->dev);

      /* All packets are assumed to be IP packets (we don't have a choice..
       * there is no Ethernet header containing the EtherType).  So pass the
       * received packet on for IP processing -- but only if it is big
       * enough to hold an IP header.
       */

#ifdef CONFIG_NET_IPv4
      if ((IPv4BUF->vhl & IP_VERSION_MASK) == IPv4_VERSION)
        {
          NETDEV_RXIPV4(&priv->dev);
          ipv4_input(&priv->dev);

          /* If the above function invocation resulted in data that should
           * be sent out on the network, the field  d_len will set to a
           * value > 0.  NOTE that we are transmitting using the RX buffer!
           */

          if (priv->dev.d_len > 0)
            {
              slip_transmit(priv);
            }
        }
      else
#endif
#ifdef CONFIG_NET_IPv6
      if ((IPv6BUF->vtc & IP_VERSION_MASK) == IPv6_VERSION)
        {
          NETDEV_RXIPV6(&priv->dev);
          ipv6_input(&priv->dev);

          /* If the above function invocation resulted in data that should
           * be sent out on the network, the field  d_len will set to a
           * value > 0.  NOTE that we are transmitting using the RX buffer!
           */

          if (priv->dev.d_len > 0)
            {
              slip_transmit(priv);
            }
        }
      else
#endif
        {
          NETDEV_RXERRORS(&priv->dev);
        }

      net_unlock();
      slip_semgive(priv);
    }

  /* We won't get here */

  return OK;
}

/****************************************************************************
 * Name: slip_ifup
 *
 * Description:
 *   NuttX Callback: Bring up the Ethernet interface when an IP address is
 *   provided
 *
 * Input Parameters:
 *   dev  - Reference to the NuttX driver state structure
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *
 ****************************************************************************/

static int slip_ifup(FAR struct net_driver_s *dev)
{
  FAR struct slip_driver_s *priv = (FAR struct slip_driver_s *)dev->d_private;

#ifdef CONFIG_NET_IPv4
  nerr("Bringing up: %d.%d.%d.%d\n",
       dev->d_ipaddr & 0xff, (dev->d_ipaddr >> 8) & 0xff,
       (dev->d_ipaddr >> 16) & 0xff, dev->d_ipaddr >> 24);
#endif
#ifdef CONFIG_NET_IPv6
  ninfo("Bringing up: %04x:%04x:%04x:%04x:%04x:%04x:%04x:%04x\n",
        dev->d_ipv6addr[0], dev->d_ipv6addr[1], dev->d_ipv6addr[2],
        dev->d_ipv6addr[3], dev->d_ipv6addr[4], dev->d_ipv6addr[5],
        dev->d_ipv6addr[6], dev->d_ipv6addr[7]);
#endif

  /* Mark the interface up */

  priv->bifup = true;
  return OK;
}

/****************************************************************************
 * Name: slip_ifdown
 *
 * Description:
 *   NuttX Callback: Stop the interface.
 *
 * Input Parameters:
 *   dev  - Reference to the NuttX driver state structure
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *
 ****************************************************************************/

static int slip_ifdown(FAR struct net_driver_s *dev)
{
  FAR struct slip_driver_s *priv = (FAR struct slip_driver_s *)dev->d_private;

  /* Mark the device "down" */

  priv->bifup = false;
  return OK;
}

/****************************************************************************
 * Name: slip_txavail
 *
 * Description:
 *   Driver callback invoked when new TX data is available.  This is a
 *   stimulus perform an out-of-cycle poll and, thereby, reduce the TX
 *   latency.
 *
 * Input Parameters:
 *   dev  - Reference to the NuttX driver state structure
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

static int slip_txavail(FAR struct net_driver_s *dev)
{
  FAR struct slip_driver_s *priv = (FAR struct slip_driver_s *)dev->d_private;

  /* Ignore the notification if the interface is not yet up */

  if (priv->bifup)
    {
      /* Wake up the TX polling thread */

      priv->txnodelay = true;
      (void)nxsig_kill(priv->txpid, SIGALRM);
    }

  return OK;
}

/****************************************************************************
 * Name: slip_addmac
 *
 * Description:
 *   NuttX Callback: Add the specified MAC address to the hardware multicast
 *   address filtering
 *
 * Input Parameters:
 *   dev  - Reference to the NuttX driver state structure
 *   mac  - The MAC address to be added
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *
 ****************************************************************************/

#ifdef CONFIG_NET_MCASTGROUP
static int slip_addmac(FAR struct net_driver_s *dev, FAR const uint8_t *mac)
{
  FAR struct slip_driver_s *priv = (FAR struct slip_driver_s *)dev->d_private;

  /* Add the MAC address to the hardware multicast routing table */

  return OK;
}
#endif

/****************************************************************************
 * Name: slip_rmmac
 *
 * Description:
 *   NuttX Callback: Remove the specified MAC address from the hardware
 *   multicast address filtering
 *
 * Input Parameters:
 *   dev  - Reference to the NuttX driver state structure
 *   mac  - The MAC address to be removed
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *
 ****************************************************************************/

#ifdef CONFIG_NET_MCASTGROUP
static int slip_rmmac(FAR struct net_driver_s *dev, FAR const uint8_t *mac)
{
  FAR struct slip_driver_s *priv = (FAR struct slip_driver_s *)dev->d_private;

  /* Add the MAC address to the hardware multicast routing table */

  return OK;
}
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: slip_initialize
 *
 * Description:
 *   Instantiate a SLIP network interface.
 *
 * Input Parameters:
 *   intf    - In the case where there are multiple SLIP interfaces, this
 *             value identifies which is to be initialized. The number of
 *             possible SLIP interfaces is determined by
 *   devname - This is the path to the serial device that will support SLIP.
 *             For example, this might be "/dev/ttyS1"
 *
 * Returned Value:
 *   OK on success; Negated errno on failure.
 *
 * Assumptions:
 *
 ****************************************************************************/

int slip_initialize(int intf, FAR const char *devname)
{
  FAR struct slip_driver_s *priv;
  char buffer[8];
  FAR char *argv[2];

  /* Get the interface structure associated with this interface number. */

  DEBUGASSERT(intf < CONFIG_NET_SLIP_NINTERFACES);
  priv = &g_slip[intf];

  /* Initialize the driver structure */

  memset(priv, 0, sizeof(struct slip_driver_s));
  priv->dev.d_ifup    = slip_ifup;     /* I/F up (new IP address) callback */
  priv->dev.d_ifdown  = slip_ifdown;   /* I/F down callback */
  priv->dev.d_txavail = slip_txavail;  /* New TX data callback */
#ifdef CONFIG_NET_MCASTGROUP
  priv->dev.d_addmac  = slip_addmac;   /* Add multicast MAC address */
  priv->dev.d_rmmac   = slip_rmmac;    /* Remove multicast MAC address */
#endif
  priv->dev.d_private = priv;          /* Used to recover private state from dev */

  /* Open the device */

  priv->fd            = nx_open(devname, O_RDWR, 0666);
  if (priv->fd < 0)
    {
      nerr("ERROR: Failed to open %s: %d\n", devname, priv->fd);
      return priv->fd;
    }

  /* Initialize the wait semaphore */

  nxsem_init(&priv->waitsem, 0, 0);
  nxsem_setprotocol(&priv->waitsem, SEM_PRIO_NONE);

  /* Start the SLIP receiver kernel thread */

  snprintf(buffer, 8, "%d", intf);
  argv[0] = buffer;
  argv[1] = NULL;

  priv->rxpid = kthread_create("rxslip", CONFIG_NET_SLIP_DEFPRIO,
                               CONFIG_NET_SLIP_STACKSIZE, (main_t)slip_rxtask,
                               (FAR char * const *)argv);
  if (priv->rxpid < 0)
    {
      nerr("ERROR: Failed to start receiver task\n");
      return priv->rxpid;
    }

  /* Wait and make sure that the receive task is started. */

  slip_semtake(priv);

  /* Start the SLIP transmitter kernel thread */

  priv->txpid = kthread_create("txslip", CONFIG_NET_SLIP_DEFPRIO,
                               CONFIG_NET_SLIP_STACKSIZE, (main_t)slip_txtask,
                               (FAR char * const *)argv);
  if (priv->txpid < 0)
    {
      nerr("ERROR: Failed to start receiver task\n");
      return priv->txpid;
    }

  /* Wait and make sure that the transmit task is started. */

  slip_semtake(priv);

  /* Bump the semaphore count so that it can now be used as a mutex */

  slip_semgive(priv);

  /* Register the device with the OS so that socket IOCTLs can be performed */

  (void)netdev_register(&priv->dev, NET_LL_SLIP);

  /* When the RX and TX tasks were created, the TTY file descriptor was
   * dup'ed for each task.  This task no longer needs the file descriptor
   * and we can safely close it.
   */

  close(priv->fd);
  return OK;
}

#endif /* CONFIG_NET && CONFIG_NET_SLIP */
