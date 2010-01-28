/*
 * dec21041.c
 *
 * This file contains an ethernet device driver for DEC  21140A
 * fast ethernet controllers as emulated by VirtualPC 2007. It is not 
 * intended to support the real card, as much more error checking
 * and testing would be needed. It supports both bridged and NAT mode.
 *
 * Created:	Mar 2008 by Nicolas Tittley <first.last@ google's mail>
 */

#include "../drivers.h"

#include <assert.h>
#include <ibm/pci.h>
#include <minix/syslib.h>
#include <minix/endpoint.h>
#include <minix/com.h>
#include <minix/sef.h>
#include <minix/ds.h>
#include <net/hton.h>
#include <net/gen/ether.h>
#include <net/gen/eth_io.h>
#include <stdlib.h>

#include "dec21140A.h"


_PROTOTYPE( PRIVATE u32_t io_inl,            (u16_t);                        );
_PROTOTYPE( PRIVATE void  io_outl,           (u16_t, u32_t);                 );
_PROTOTYPE( PRIVATE void  do_conf,           (message *);                    );
_PROTOTYPE( PRIVATE void  do_fkey,           (message *);                    );
_PROTOTYPE( PRIVATE void  do_get_name,       (message *);                    );
_PROTOTYPE( PRIVATE void  do_get_stat_s,     (message *);                    );
_PROTOTYPE( PRIVATE void  do_interrupt,      (dpeth_t *);                    );
_PROTOTYPE( PRIVATE void  do_reply,          (dpeth_t *, int, int);          );
_PROTOTYPE( PRIVATE void  do_vread_s,        (message *, int);               );
_PROTOTYPE( PRIVATE void  do_watchdog,       (void *);                       );

_PROTOTYPE( PRIVATE void  de_update_conf,    (dpeth_t *);                    );
_PROTOTYPE( PRIVATE int   de_probe,          (dpeth_t *);                    );
_PROTOTYPE( PRIVATE void  de_conf_addr,      (dpeth_t *);                    );
_PROTOTYPE( PRIVATE void  de_first_init,     (dpeth_t *);                    );
_PROTOTYPE( PRIVATE void  de_reset,          (dpeth_t *);                    );
_PROTOTYPE( PRIVATE void  de_hw_conf,        (dpeth_t *);                    );
_PROTOTYPE( PRIVATE void  de_start,          (dpeth_t *);                    );
_PROTOTYPE( PRIVATE void  de_setup_frame,    (dpeth_t *);                    );
_PROTOTYPE( PRIVATE u16_t de_read_rom,       (dpeth_t *, u8_t, u8_t);        );
_PROTOTYPE( PRIVATE int   de_calc_iov_size,  (iovec_dat_s_t *);              );
_PROTOTYPE( PRIVATE void  de_next_iov,       (iovec_dat_s_t *);              );
_PROTOTYPE( PRIVATE void  do_vwrite_s,       (message *, int);               );
_PROTOTYPE( PRIVATE void  de_get_userdata_s, (int, cp_grant_id_t,
					     vir_bytes, int, void *);        );

/* Error messages */
static char str_CopyErrMsg[]  = "unable to read/write user data";
static char str_PortErrMsg[]  = "illegal port";
static char str_RecvErrMsg[]  = "receive failed";
static char str_SendErrMsg[]  = "send failed";
static char str_SizeErrMsg[]  = "illegal packet size";
static char str_TypeErrMsg[]  = "illegal message type";
static char str_UmapErrMsg[]  = "Unable to sys_umap";
static char str_BusyErrMsg[]  = "Send/Recv failed: busy";
static char str_StatErrMsg[]  = "Unable to send stats";
static char str_AlignErrMsg[] = "Bad align of buffer/descriptor";
static char str_DevName[]     = "dec21140A:eth#?";

extern int     errno;
static dpeth_t de_table[DE_PORT_NR];
static char    *progname;

int sef_cb_init(int type, sef_init_info_t *info)
{
  int r;
  int fkeys, sfkeys;
  endpoint_t tasknr;
  /* Request function key for debug dumps */
  fkeys = sfkeys = 0; bit_set(sfkeys, DE_FKEY);
  if ((fkey_map(&fkeys, &sfkeys)) != OK) 
    printf("%s: error using Shift+F%d key(%d)\n", str_DevName, DE_FKEY, errno);

  /* Try to notify inet that we are present (again) */
  r = ds_retrieve_label_num("inet", &tasknr);
  if (r == OK)
    notify(tasknr);
  else if(r != ESRCH)
    printf("%s unable to notify inet: %d\n", str_DevName, r);
  return r;
}

/*===========================================================================*
 *				main					     *
 *===========================================================================*/
int main(int argc, char *argv[])
{
  dpeth_t *dep;
  message m;
  int r;

  (progname=strrchr(argv[0],'/')) ? progname++ : (progname=argv[0]);

  env_setargs(argc, argv);

  sef_setcb_init_fresh(sef_cb_init);
  sef_setcb_init_restart(sef_cb_init);
  sef_startup();
  
  while (TRUE)
    {
      if ((r= sef_receive(ANY, &m)) != OK)
	panic(str_DevName, "minix msg sef_receive failed", r);

		if(is_notify(m.m_type)) {
			switch(_ENDPOINT_P(m.m_source)) {
				case RS_PROC_NR:
					notify(m.m_source);
					break;
				case CLOCK:
					do_watchdog(&m);
					break;

	case HARDWARE:
	  for (dep = de_table; dep < &de_table[DE_PORT_NR]; dep += 1) {
	    if (dep->de_mode == DEM_ENABLED) {
	      do_interrupt(dep);
	      if (dep->de_flags & (DEF_ACK_SEND | DEF_ACK_RECV))
		do_reply(dep, OK, TRUE);
	      sys_irqenable(&dep->de_hook);
	    }
	  }
	  break;
	 case PM_PROC_NR:
	 	exit(0);
	 	break;
	 default:
	 	printf("ignoring notify from %d\n", m.m_source);
	 	break;
			}
			continue;
		}
      
      switch (m.m_type)
	{
	case DL_WRITEV_S:  do_vwrite_s(&m, FALSE); break;
	case DL_READV_S:   do_vread_s(&m, FALSE);  break;	  
	case DL_CONF:      do_conf(&m);            break;  
	case DL_GETSTAT_S: do_get_stat_s(&m);      break;
	case DL_GETNAME:   do_get_name(&m);        break;
	case DL_STOP:      /* nothing */           break;

	default:  
		printf("message 0x%lx; %d from %d\n",
			m.m_type, m.m_type-DL_RQ_BASE, m.m_source);
		panic(str_DevName, "illegal message", m.m_type);
	}
    }
}

PRIVATE void do_get_stat_s(message * mp)
{
  int port, rc;
  dpeth_t *dep;

  port = mp->DL_PORT;
  if (port < 0 || port >= DE_PORT_NR)
	panic(str_DevName, str_PortErrMsg, port);

  dep = &de_table[port];
  dep->de_client = mp->DL_PROC;

  if ((rc = sys_safecopyto(mp->DL_PROC, mp->DL_GRANT, 0,
			(vir_bytes)&dep->de_stat,
			(vir_bytes) sizeof(dep->de_stat), 0)) != OK)
        panic(str_DevName, str_CopyErrMsg, rc);

  mp->m_type = DL_STAT_REPLY;
  mp->DL_PORT = port;
  mp->DL_STAT = OK;
  rc = send(mp->m_source, mp);
  if( rc != OK )
    panic(str_DevName, str_StatErrMsg, rc);
  return;
}

PRIVATE void do_conf(message * mp)
{
  int port;
  dpeth_t *dep;
  message reply_mess;

  port = mp->DL_PORT;
  if (port >= 0 && port < DE_PORT_NR) {

    dep = &de_table[port];
    strncpy(dep->de_name, str_DevName, strlen(str_DevName));
    dep->de_name[strlen(dep->de_name)-1] = '0' + port;

    if (dep->de_mode == DEM_DISABLED) {
      de_update_conf(dep); 
      pci_init();
      if (dep->de_mode == DEM_ENABLED && !de_probe(dep)) {
	printf("%s: warning no ethernet card found at 0x%04X\n",
	       dep->de_name, dep->de_base_port);
	dep->de_mode = DEM_DISABLED;
      }
    }

    /* 'de_mode' may change if probe routines fail, test again */
    switch (dep->de_mode) {

    case DEM_DISABLED:
      port = ENXIO;       /* Device is OFF or hardware probe failed */
      break;

    case DEM_ENABLED:
      if (dep->de_flags == DEF_EMPTY) {
	de_first_init(dep);
	dep->de_flags |= DEF_ENABLED;
	de_reset(dep);
	de_hw_conf(dep);
	de_setup_frame(dep);
	de_start(dep);
      }

      /* TODO CHECK PROMISC AND MULTI */
      dep->de_flags &= NOT(DEF_PROMISC | DEF_MULTI | DEF_BROAD);
      if (mp->DL_MODE & DL_PROMISC_REQ)
	dep->de_flags |= DEF_PROMISC | DEF_MULTI | DEF_BROAD;
      if (mp->DL_MODE & DL_MULTI_REQ) dep->de_flags |= DEF_MULTI;
      if (mp->DL_MODE & DL_BROAD_REQ) dep->de_flags |= DEF_BROAD;
      dep->de_client = mp->m_source;
      break;

    case DEM_SINK:
      DEBUG(printf("%s running in sink mode\n", str_DevName));
      memset(dep->de_address.ea_addr, 0, sizeof(ether_addr_t));
      de_conf_addr(dep);
      break;

    default:	break;
    }
  } else			/* Port number is out of range */
    port = ENXIO;

  reply_mess.m_type = DL_CONF_REPLY;
  reply_mess.m3_i1 = port;
  reply_mess.m3_i2 = DE_PORT_NR;
   *(ether_addr_t *) reply_mess.m3_ca1 = dep->de_address;
  
  if (send(mp->m_source, &reply_mess) != OK)
    panic(str_DevName, str_SendErrMsg, mp->m_source);

  return;
}


PRIVATE void do_get_name(mp)
message *mp;
{
  int r;
  strncpy(mp->DL_NAME, progname, sizeof(mp->DL_NAME));
  mp->DL_NAME[sizeof(mp->DL_NAME)-1]= '\0';
  mp->m_type= DL_NAME_REPLY;
  r = send(mp->m_source, mp);
  if (r!= OK)
    panic(str_DevName, "do_getname: send failed", r);
}

PRIVATE void do_reply(dpeth_t * dep, int err, int may_block)
{
  message reply;
  int status = FALSE;

  if (dep->de_flags & DEF_ACK_SEND) status |= DL_PACK_SEND;
  if (dep->de_flags & DEF_ACK_RECV) status |= DL_PACK_RECV;

  reply.m_type = DL_TASK_REPLY;
  reply.DL_PORT = dep - de_table;
  reply.DL_PROC = dep->de_client;
  reply.DL_STAT = status | ((u32_t) err << 16);
  reply.DL_COUNT = dep->de_read_s;
  reply.DL_CLCK = 0;

  status = send(dep->de_client, &reply);

  if(status == ELOCKED && may_block){
    /*printf("Warning: Dec21041 send lock prevented\n\n");*/
    return;
  }

  if(status < 0)
    panic(dep->de_name, str_SendErrMsg, status);

  dep->de_read_s = 0;
  dep->de_flags &= NOT(DEF_ACK_SEND | DEF_ACK_RECV);
  return;
}

PRIVATE void do_watchdog(void *message)
{
  /* nothing here yet */
  return;
}

PRIVATE int de_probe(dpeth_t *dep){
  int i, r, devind;
  u16_t vid, did, temp16;

  DEBUG(printf("PROBING..."));
  
  r= pci_first_dev(&devind, &vid, &did);
  if (r == 0)
    return FALSE;

  for(;;)
    {
      if ( DEC21140A_VID == vid && 
	   DEC21140A_DID == did)
	break;

      r= pci_next_dev(&devind, &vid, &did);
      if (!r)
	return FALSE;
    }

  pci_reserve(devind);

  dep->de_base_port = pci_attr_r32(devind, PCI_BAR) & 0xffffffe0;
  dep->de_irq = pci_attr_r8(devind, PCI_ILR);

  if (dep->de_base_port < DE_MIN_BASE_ADDR)
    panic(str_DevName,"de_probe: base address invalid ", dep->de_base_port);

  DEBUG(printf("%s: using I/O address 0x%lx, IRQ %d\n",
	       dep->de_name, (unsigned long)dep->de_base_port, 
	       dep->de_irq));

  dep->de_type = pci_attr_r8(devind, PCI_REV);

  /* device validation. We support only the DEC21140A */
  if(dep->de_type != DEC_21140A){
    dep->de_type = DE_TYPE_UNKNOWN;
    printf("%s: unsupported device\n", str_DevName);
    return FALSE;
  }

  de_reset(dep);

  DEBUG(printf("Reading SROM...\n"));

  for(i=0;i<(1<<SROM_BITWIDTH)-1;i++){
    temp16 = de_read_rom(dep, i, SROM_BITWIDTH);
    dep->srom[i*2] = temp16 & 0xFF;
    dep->srom[i*2+1] = temp16 >> 8;
  }

  /* TODO: validate SROM content */
  /* acquire MAC addr */
  DEBUG(printf("Using MAC addr= "));
  for(i=0;i<6;i++){
    dep->de_address.ea_addr[i] = dep->srom[i+DE_SROM_EA_OFFSET];
    DEBUG(printf("%02X%c",dep->de_address.ea_addr[i],i!=5?'-':'\n'));
  }
  DEBUG(printf("probe success\n"));
  return TRUE;
}

PRIVATE u16_t de_read_rom(dpeth_t *dep, u8_t addr, u8_t nbAddrBits){
  u16_t retVal = 0;
  int i;
  u32_t csr = 0;
  u32_t csr2 = 0; /* csr2 is used to hold constant values that are
		     setup in the init phase, it makes this a little
		     more readable, the following macro is also just
		     to clear up the code a little.*/

  #define EMIT do { io_outl(CSR_ADDR(dep, CSR9), csr | csr2); io_outl(CSR_ADDR(dep, CSR1), 0);} while(0)

  /* init */
  csr = 0;                 EMIT;
  csr = CSR9_SR;           EMIT;
  csr = CSR9_SR | CSR9_RD; EMIT;

  csr2 = CSR9_SR | CSR9_RD;
  csr = 0;                 EMIT;
  csr2 |= CSR9_CS;

  csr = 0;                 EMIT;
  csr = CSR9_SRC;          EMIT;
  csr = 0;                 EMIT;

  /* cmd 110 - Read */
  csr = CSR9_DI;            EMIT;
  csr = CSR9_DI | CSR9_SRC; EMIT;
  csr = CSR9_DI;            EMIT;
  csr = CSR9_DI | CSR9_SRC; EMIT;
  csr = CSR9_DI;            EMIT;
  csr = 0;                  EMIT;
  csr = CSR9_SRC;           EMIT;
  csr = 0;                  EMIT;

  /* addr to read */
  for(i=nbAddrBits;i!=0;i--){
    csr = (addr&(1<<(i-1))) != 0 ? CSR9_DI : 0;  EMIT;
    csr ^= CSR9_SRC; EMIT;
    csr ^= CSR9_SRC; EMIT;
  }

  /* actual read */
  retVal=0;
  for(i=0;i<16;i++){
    retVal <<= 1;
    csr = CSR9_SRC; EMIT;
    retVal |= (io_inl(CSR_ADDR(dep, CSR9)) & CSR9_DO) == 0 ? 0 : 1;
    csr = 0; EMIT;
  }

  /* clean up */
  csr = 0;                 EMIT;

#undef EMIT
  return retVal;
}

static void de_update_conf(dpeth_t * dep)
{
  static char dpc_fmt[] = "x:d:x";
  long val;

  dep->de_mode = DEM_ENABLED;
  switch (env_parse("DEETH0", dpc_fmt, 0, &val, 0x000L, 0x3FFL)) {
  case EP_OFF:	dep->de_mode = DEM_DISABLED;	break;
  case EP_ON:  dep->de_mode = DEM_SINK; break;
  }
  dep->de_base_port = 0;
  
  return;
}

PRIVATE void do_vread_s(message * mp, int from_int)
{
  char *buffer;
  u32_t size;
  int r, bytes, ix = 0;
  dpeth_t *dep = NULL;
  de_loc_descr_t *descr = NULL;
  iovec_dat_s_t *iovp = NULL;

  if (mp->DL_PORT < 0 || mp->DL_PORT >= DE_PORT_NR)
    panic(dep->de_name, str_PortErrMsg, mp->DL_PORT);

  dep = &de_table[mp->DL_PORT];
  dep->de_client = mp->DL_PROC;

  if (dep->de_mode == DEM_ENABLED) {    

    descr = &dep->descr[DESCR_RECV][dep->cur_descr[DESCR_RECV]];  

    /* check if packet is in the current descr and only there */
    if(  !( !(descr->descr->des[DES0] & DES0_OWN) &&  
	    (descr->descr->des[DES0] & DES0_FS)   &&
	    (descr->descr->des[DES0] & DES0_LS)     ))
      goto suspend;


    /*TODO: multi-descr msgs...*/
    /* We only support packets contained in a single descriptor.
       Setting the descriptor buffer size to less then
       ETH_MAX_PACK_SIZE will result in multi-descriptor
       packets that we won't be able to handle 
    */
    assert(!(descr->descr->des[DES0]&DES0_OWN));
    assert(descr->descr->des[DES0]&DES0_FS);
    assert(descr->descr->des[DES0]&DES0_LS);

    /* Check for abnormal messages. We assert here
       because this driver is for a virtualized 
       envrionment where we will not get bad packets
    */
    assert(!(descr->descr->des[DES0]&DES0_ES));
    assert(!(descr->descr->des[DES0]&DES0_RE));


    /* Setup the iovec entry to allow copying into
       client layer
    */
    dep->de_read_iovec.iod_proc_nr = mp->DL_PROC;
    de_get_userdata_s(mp->DL_PROC, (vir_bytes) mp->DL_GRANT, 0,
		      mp->DL_COUNT, dep->de_read_iovec.iod_iovec);
    dep->de_read_iovec.iod_iovec_s = mp->DL_COUNT;
    dep->de_read_iovec.iod_grant = (vir_bytes) mp->DL_GRANT;
    dep->de_read_iovec.iod_iovec_offset = 0;
    size = de_calc_iov_size(&dep->de_read_iovec);
    if (size < ETH_MAX_PACK_SIZE) 
      panic(str_DevName, str_SizeErrMsg, size);

    /* Copy buffer to user area  and clear ownage */
    size = (descr->descr->des[DES0]&DES0_FL)>>DES0_FL_SHIFT;

    /*TODO: Complain to MS */
    /*HACK: VPC2007 returns packet of invalid size. Ethernet standard
      specify 46 bytes as the minimum for valid payload. However, this is 
      artificial in so far as for certain packet types, notably ARP, less
      then 46 bytes are needed to contain the full information. In a non 
      virtualized environment the 46 bytes rule is enforced in order to give
      guarantee in the collison detection scheme. Of course, this being a 
      driver for a VPC2007, we won't have collisions and I can only suppose
      MS decided to cut packet size to true minimum, regardless of the 
      46 bytes payload standard. Note that this seems to not happen in 
      bridged mode. Note also, that the card does not return runt or 
      incomplete frames to us, so this hack is safe
    */    
    if(size<60){
      bzero(&descr->buf1[size], 60-size);
      size=60;
    }
    /* End ugly hack */

    iovp = &dep->de_read_iovec;
    buffer = descr->buf1;
    dep->bytes_rx += size;
    dep->de_stat.ets_packetR++;
    dep->de_read_s = size;

    do {   
      bytes = iovp->iod_iovec[ix].iov_size;	/* Size of buffer */
      if (bytes >= size) 
	bytes = size;

      r= sys_safecopyto(iovp->iod_proc_nr, iovp->iod_iovec[ix].iov_grant, 0,
			(vir_bytes)buffer, bytes, D);
      if (r != OK)
	panic(str_DevName, str_CopyErrMsg, r);
      buffer += bytes;
      
      if (++ix >= IOVEC_NR) {	/* Next buffer of IO vector */
	de_next_iov(iovp);
	ix = 0;
      }
    } while ((size -= bytes) > 0);

    descr->descr->des[DES0]=DES0_OWN;
    dep->cur_descr[DESCR_RECV]++;
    if(dep->cur_descr[DESCR_RECV] >= DE_NB_RECV_DESCR)
      dep->cur_descr[DESCR_RECV] = 0;	  

    DEBUG(printf("Read returned size = %d\n", size));

    /* Reply information */
    dep->de_flags |= DEF_ACK_RECV;
    dep->de_flags &= NOT(DEF_READING);
  }

  if(!from_int){
    do_reply(dep, OK, FALSE);
  }
  return;

 suspend:
  if(from_int){
    assert(dep->de_flags & DEF_READING);
    return;
  }

  assert(!(dep->de_flags & DEF_READING));
  dep->rx_return_msg = *mp;
  dep->de_flags |= DEF_READING;
  do_reply(dep, OK, FALSE);
  return;
}

PRIVATE void de_conf_addr(dpeth_t * dep)
{
  static char ea_fmt[] = "x:x:x:x:x:x";
  char ea_key[16];
  int ix;
  long val;

  /* TODO: should be configurable... */
  strcpy(ea_key, "DEETH0");
  strcat(ea_key, "_EA");

  for (ix = 0; ix < SA_ADDR_LEN; ix++) {
	val = dep->de_address.ea_addr[ix];
	if (env_parse(ea_key, ea_fmt, ix, &val, 0x00L, 0xFFL) != EP_SET)
		break;
	dep->de_address.ea_addr[ix] = val;
  }

  if (ix != 0 && ix != SA_ADDR_LEN)
	env_parse(ea_key, "?", 0, &val, 0L, 0L);
  return;
}

PRIVATE void do_fkey(message *mp)
{
  dpeth_t *dep;
  int port,i;

  printf("\n");
  for (port = 0, dep = de_table; port < DE_PORT_NR; port += 1, dep += 1) {
    if (dep->de_mode == DEM_DISABLED) continue;
    printf("%s status:\n", dep->de_name);
    printf("hwaddr: ");
    for(i=0;i<6;i++)
      printf("%02X%c",dep->de_address.ea_addr[i], i!=5?':':'\n');
    printf("Tx packets: %-16d Tx kb: %d.%02d\n", dep->de_stat.ets_packetT,
	   dep->bytes_tx/1024, 
	   (int)(((dep->bytes_tx%1024)/1024.0)*100));
    printf("Rx packets: %-16d Rx kb: %d.%02d\n", dep->de_stat.ets_packetR,
	   dep->bytes_rx/1024,
	   (int)(((dep->bytes_rx%1024)/1024.0)*100));
    printf("Rx errors:  %-16d Tx errors: %d\n", 
	   dep->de_stat.ets_recvErr,
	   dep->de_stat.ets_sendErr);
  }
  return;
}

PRIVATE void de_first_init(dpeth_t *dep){
  int i,j,r;
  vir_bytes descr_vir = dep->sendrecv_descr_buf;
  vir_bytes buffer_vir = dep->sendrecv_buf;
  de_descr_t *phys_descr;
  de_loc_descr_t *loc_descr;
  u32_t temp;


  for(i=0;i<2;i++){
    loc_descr = &dep->descr[i][0];
    for(j=0; j < (i==DESCR_RECV ? DE_NB_RECV_DESCR : DE_NB_SEND_DESCR); j++){

      /* assign buffer space for descriptor */
      loc_descr->descr = descr_vir;
      descr_vir += sizeof(de_descr_t);

      /* assign space for buffer */
      loc_descr->buf1 = buffer_vir; 
      buffer_vir += (i==DESCR_RECV ? DE_RECV_BUF_SIZE : DE_SEND_BUF_SIZE);
      loc_descr->buf2 = 0;
      loc_descr++;
    }
  }

  /* Now that we have buffer space and descriptors, we need to
     obtain their physical address to pass to the hardware
  */
  for(i=0;i<2;i++){
    loc_descr = &dep->descr[i][0];
    temp = (i==DESCR_RECV ? DE_RECV_BUF_SIZE : DE_SEND_BUF_SIZE);
    for(j=0; j < (i==DESCR_RECV ? DE_NB_RECV_DESCR : DE_NB_SEND_DESCR); j++){
      /* translate buffers physical address */
      r = sys_umap(SELF, VM_D, loc_descr->buf1, temp, 
		   &(loc_descr->descr->des[DES_BUF1]));
      if(r != OK) panic(dep->de_name, "umap failed", r);      
      loc_descr->descr->des[DES_BUF2] = 0;
      memset(&loc_descr->descr->des[DES0],0,sizeof(u32_t));
      loc_descr->descr->des[DES1] = temp;
      if(j==( (i==DESCR_RECV?DE_NB_RECV_DESCR:DE_NB_SEND_DESCR)-1))
	loc_descr->descr->des[DES1] |= DES1_ER;
      if(i==DESCR_RECV)
	loc_descr->descr->des[DES0] |= DES0_OWN;
      loc_descr++;
    }
  }
  
  /* record physical location of two first descriptor */
  r = sys_umap(SELF, VM_D, dep->descr[DESCR_RECV][0].descr, 
	       sizeof(de_descr_t), &dep->sendrecv_descr_phys_addr[DESCR_RECV]);
  if(r != OK) panic(str_DevName, str_UmapErrMsg, r);

  r = sys_umap(SELF, VM_D, dep->descr[DESCR_TRAN][0].descr,
	       sizeof(de_descr_t), &dep->sendrecv_descr_phys_addr[DESCR_TRAN]);
  if(r != OK) panic(str_DevName, str_UmapErrMsg, r);

  DEBUG(printf("Descr: head tran=[%08X] head recv=[%08X]\n",
	       dep->sendrecv_descr_phys_addr[DESCR_TRAN],
	       dep->sendrecv_descr_phys_addr[DESCR_RECV]));

  /* check alignment just to be extra safe */
  for(i=0;i<2;i++){
    loc_descr = &dep->descr[i][0];
    for(j=0;j< (i==DESCR_RECV?DE_NB_RECV_DESCR:DE_NB_SEND_DESCR);j++){
      r = sys_umap(SELF, VM_D, &(loc_descr->descr), sizeof(de_descr_t), 
		   &temp);
      if(r != OK)
	panic(str_DevName, str_UmapErrMsg, r);

      if( ((loc_descr->descr->des[DES_BUF1] & 0x3) != 0) ||
	  ((loc_descr->descr->des[DES_BUF2] & 0x3) != 0) ||
	  ((temp&0x3)!=0) )
	panic(str_DevName, str_AlignErrMsg, temp);

      loc_descr++;
    }
  }
  
  /* Init default values */
  dep->cur_descr[DESCR_TRAN]=1;
  dep->cur_descr[DESCR_RECV]=0;
  dep->bytes_rx = 0;
  dep->bytes_tx = 0;
  
  /* Set the interrupt handler policy. Request interrupts not to be reenabled
   * automatically. Return the IRQ line number when an interrupt occurs.
   */
  dep->de_hook = dep->de_irq;
  sys_irqsetpolicy(dep->de_irq, 0, &dep->de_hook);
  sys_irqenable(&dep->de_hook);
}

PRIVATE void do_interrupt(dpeth_t *dep){  
  u32_t val;
  val = io_inl(CSR_ADDR(dep, CSR5));

  if(val & CSR5_AIS){
    panic(dep->de_name, "Abnormal Int CSR5=", val);
  }

  if( (dep->de_flags & DEF_READING) && (val & CSR5_RI) ){
    do_vread_s(&dep->rx_return_msg, TRUE);
  }
 
  if( (dep->de_flags & DEF_SENDING) && (val & CSR5_TI) ){
    do_vwrite_s(&dep->tx_return_msg, TRUE);
  }
  
  /* ack and reset interrupts */
  io_outl(CSR_ADDR(dep, CSR5), 0xFFFFFFFF);
  return;
}

PRIVATE void de_reset(dpeth_t *dep){
  io_outl(CSR_ADDR(dep, CSR0), CSR0_SWR);
  micro_delay(1000000);
}

PRIVATE void de_hw_conf(dpeth_t *dep){
  u32_t val;

  /* CSR0 - global host bus prop */
  val = CSR0_BAR | CSR0_CAL_8;
  io_outl(CSR_ADDR(dep, CSR0), val);

  /* CSR3 - Receive list BAR */
  val = dep->sendrecv_descr_phys_addr[DESCR_RECV];
  io_outl(CSR_ADDR(dep, CSR3), val);

  /* CSR4 - Transmit list BAR */
  val = dep->sendrecv_descr_phys_addr[DESCR_TRAN];
  io_outl(CSR_ADDR(dep, CSR4), val);

  /* CSR7 - interrupt mask */
  val = CSR7_TI | CSR7_RI | CSR7_AI;
  io_outl(CSR_ADDR(dep, CSR7), val);

  /* CSR6 - operating mode register */
  val = CSR6_MBO | CSR6_PS | CSR6_FD | CSR6_HBD | 
    CSR6_PCS | CSR6_SCR | CSR6_TR_00;
  io_outl(CSR_ADDR(dep, CSR6), val);
}

PRIVATE void de_start(dpeth_t *dep){  
  u32_t val;
  val = io_inl(CSR_ADDR(dep, CSR6)) | CSR6_ST | CSR6_SR;
  io_outl(CSR_ADDR(dep, CSR6), val);
}

PRIVATE void de_setup_frame(dpeth_t *dep){
  int i;
  u32_t val;

  /* this is not perfect... we assume pass all multicast and only
     filter non-multicast frames */
  dep->descr[DESCR_TRAN][0].buf1[0] = 0xFF;
  dep->descr[DESCR_TRAN][0].buf1[1] = 0xFF;
  dep->descr[DESCR_TRAN][0].buf1[4] = 0xFF;
  dep->descr[DESCR_TRAN][0].buf1[5] = 0xFF;
  dep->descr[DESCR_TRAN][0].buf1[8] = 0xFF;
  dep->descr[DESCR_TRAN][0].buf1[9] = 0xFF;
  for(i=1;i<16;i++){
    memset(&(dep->descr[DESCR_TRAN][0].buf1[12*i]), 0, 12);
    dep->descr[DESCR_TRAN][0].buf1[12*i+0] = dep->de_address.ea_addr[0];
    dep->descr[DESCR_TRAN][0].buf1[12*i+1] = dep->de_address.ea_addr[1];
    dep->descr[DESCR_TRAN][0].buf1[12*i+4] = dep->de_address.ea_addr[2];
    dep->descr[DESCR_TRAN][0].buf1[12*i+5] = dep->de_address.ea_addr[3];
    dep->descr[DESCR_TRAN][0].buf1[12*i+8] = dep->de_address.ea_addr[4];
    dep->descr[DESCR_TRAN][0].buf1[12*i+9] = dep->de_address.ea_addr[5];
  }

  dep->descr[DESCR_TRAN][0].descr->des[DES0] = DES0_OWN;
  dep->descr[DESCR_TRAN][0].descr->des[DES1] = DES1_SET | 
    DE_SETUP_FRAME_SIZE | DES1_IC;

  /* start transmit process to process setup frame */
  val = io_inl(CSR_ADDR(dep, CSR6)) | CSR6_ST;
  io_outl(CSR_ADDR(dep, CSR6), val); 
  io_outl(CSR_ADDR(dep, CSR1), 0xFFFFFFFF);

  return;
}

PRIVATE int de_calc_iov_size(iovec_dat_s_t * iovp){
  int size, ix;
  size = ix = 0;
  
  do{
    size += iovp->iod_iovec[ix].iov_size;
    if (++ix >= IOVEC_NR) {
      de_next_iov(iovp);
      ix = 0;
    }
  } while (ix < iovp->iod_iovec_s);
  return size;
}

PRIVATE void de_get_userdata_s(int user_proc, cp_grant_id_t grant,
	vir_bytes offset, int count, void *loc_addr){
  int rc;
  vir_bytes len;

  len = (count > IOVEC_NR ? IOVEC_NR : count) * sizeof(iovec_t);
  rc = sys_safecopyfrom(user_proc, grant, 0, (vir_bytes)loc_addr, len, D);
  if (rc != OK)
    panic(str_DevName, str_CopyErrMsg, rc);
  return;
}

PRIVATE void de_next_iov(iovec_dat_s_t * iovp){

  iovp->iod_iovec_s -= IOVEC_NR;
  iovp->iod_iovec_offset += IOVEC_NR * sizeof(iovec_t);
  de_get_userdata_s(iovp->iod_proc_nr, iovp->iod_grant, iovp->iod_iovec_offset,
	     iovp->iod_iovec_s, iovp->iod_iovec);
  return;
}

PRIVATE void do_vwrite_s(message * mp, int from_int){
  static u8_t setupDone = 0;
  int size, r, bytes, ix, totalsize;
  dpeth_t *dep = NULL;
  iovec_dat_s_t *iovp = NULL;
  de_loc_descr_t *descr = NULL;
  char *buffer = NULL;

  if( mp->DL_PORT < 0 || mp->DL_PORT >= DE_PORT_NR)	
    panic(str_DevName, str_PortErrMsg, mp->DL_PORT);

  dep = &de_table[mp->DL_PORT];
  dep->de_client = mp->DL_PROC;

  if (dep->de_mode == DEM_ENABLED) {
    
    if (!from_int && (dep->de_flags & DEF_SENDING))
      panic(str_DevName, str_BusyErrMsg, NO_NUM);

    descr = &dep->descr[DESCR_TRAN][dep->cur_descr[DESCR_TRAN]];

    if(( descr->descr->des[DES0] & DES0_OWN)!=0)
      goto suspend;

    if(!setupDone && (dep->cur_descr[DESCR_TRAN] == 0) ){
      dep->descr[DESCR_TRAN][0].descr->des[DES0] = 0;
      setupDone=1;
    }

    buffer = descr->buf1;
    iovp = &dep->de_write_iovec;
    iovp->iod_proc_nr = mp->DL_PROC;
    de_get_userdata_s(mp->DL_PROC, mp->DL_GRANT, 0,
		      mp->DL_COUNT, iovp->iod_iovec);
    iovp->iod_iovec_s = mp->DL_COUNT;
    iovp->iod_grant = (vir_bytes) mp->DL_GRANT;
    iovp->iod_iovec_offset = 0;
    totalsize = size = de_calc_iov_size(iovp);
    if (size < ETH_MIN_PACK_SIZE || size > ETH_MAX_PACK_SIZE)
      panic(str_DevName, str_SizeErrMsg, size);

    dep->bytes_tx += size;
    dep->de_stat.ets_packetT++;

    ix=0;
    do {
      bytes = iovp->iod_iovec[ix].iov_size;
      if (bytes >= size) 
	bytes = size;		

      r= sys_safecopyfrom(iovp->iod_proc_nr, iovp->iod_iovec[ix].iov_grant,
			  0, (vir_bytes)buffer, bytes, D);
      if (r != OK)
	panic(str_DevName, str_CopyErrMsg, r);
      buffer += bytes;

      if (++ix >= IOVEC_NR) {	
	de_next_iov(iovp);
	ix = 0;
      }
    } while ((size -= bytes) > 0);

    descr->descr->des[DES1] = (descr->descr->des[DES1]&DES1_ER) | 
      DES1_FS | DES1_LS | DES1_IC | totalsize;
    descr->descr->des[DES0] = DES0_OWN;

    dep->cur_descr[DESCR_TRAN]++;
    if(dep->cur_descr[DESCR_TRAN] >= DE_NB_SEND_DESCR)
      dep->cur_descr[DESCR_TRAN] = 0;

    io_outl(CSR_ADDR(dep, CSR1), 0xFFFFFFFF);
  }
  
  dep->de_flags |= DEF_ACK_SEND;
  if(from_int){
    dep->de_flags &= NOT(DEF_SENDING);
    return;
  }
  do_reply(dep, OK, FALSE);
  return;

 suspend:
  if(from_int)
    panic(str_DevName, "should not happen", 0);

  dep->de_stat.ets_transDef++;
  dep->de_flags |= DEF_SENDING;
  dep->de_stat.ets_transDef++;
  dep->tx_return_msg = *mp;
  do_reply(dep, OK, FALSE);
}

PRIVATE void warning(const char *type, int err){
  printf("Warning: %s sys_%s failed (%d)\n", str_DevName, type, err);
  return;
}

PRIVATE u32_t io_inl(u16_t port){
  u32_t value;
  int rc;
  if ((rc = sys_inl(port, &value)) != OK) warning("inl", rc);
  return value;
}

PRIVATE void io_outl(u16_t port, u32_t value){
  int rc;
  if ((rc = sys_outl(port, value)) != OK) warning("outl", rc);
  return;
}
