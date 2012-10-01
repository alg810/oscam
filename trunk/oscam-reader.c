#include "globals.h"
#include "module-led.h"
#include "reader-common.h"
#include "csctapi/ifd_sc8in1.h"

static char *debug_mask_txt(int mask) {
	switch (mask) {
		case D_EMM    : return "EMM: ";
		case D_IFD    : return "IFD: ";
		case D_TRACE  : return "TRACE: ";
		case D_DEVICE : return "IO: ";
		default       : return "";
	}
}

static char *reader_desc_txt(struct s_reader *reader) {
	if (reader->csystem.desc)
		return reader->csystem.desc;
	else if (reader->crdr.desc)
		return reader->crdr.desc;
	else if (reader->ph.desc)
		return reader->ph.desc;
	else
		return reader_get_type_desc(reader, 1);
}

static char *format_sensitive(char *result, int remove_sensitive) {
	// Filter sensitive information
	int i, n = strlen(result), p = 0;
	if (remove_sensitive) {
		int in_sens = 0;
		for (i = 0; i < n; i++) {
			switch(result[i]) {
				case '{': in_sens = 1; continue;
				case '}': in_sens = 0; break;
			}
			if (in_sens)
				result[i] = '#';
		}
	}
	// Filter sensitive markers
	for (i = 0; i < n; i++) {
		if (result[i] == '{' || result[i] == '}')
			continue;
		result[p++] = result[i];
	}
	result[p] = '\0';
	return result;
}

void rdr_log(struct s_reader * reader, char *fmt, ...) {
	char txt[256];
	va_list args;
	va_start(args, fmt);
	vsnprintf(txt, sizeof(txt), fmt, args);
	va_end(args);
	cs_log("%s [%s] %s", reader->label, reader_desc_txt(reader), txt);
}

void rdr_log_sensitive(struct s_reader * reader, char *fmt, ...) {
	char txt[256];
	va_list args;
	va_start(args, fmt);
	vsnprintf(txt, sizeof(txt), fmt, args);
	va_end(args);
	format_sensitive(txt, log_remove_sensitive);
	rdr_log(reader, "%s", txt);
}

void rdr_debug_mask(struct s_reader * reader, uint16_t mask, char *fmt, ...) {
	if (config_WITH_DEBUG()) {
		char txt[2048];
		va_list args;
		va_start(args, fmt);
		vsnprintf(txt, sizeof(txt), fmt, args);
		va_end(args);
		cs_debug_mask(mask, "%s [%s] %s%s", reader->label, reader_desc_txt(reader), debug_mask_txt(mask), txt);
	}
}

void rdr_debug_mask_sensitive(struct s_reader * reader, uint16_t mask, char *fmt, ...) {
	if (config_WITH_DEBUG()) {
		char txt[2048];
		va_list args;
		va_start(args, fmt);
		vsnprintf(txt, sizeof(txt), fmt, args);
		va_end(args);
		format_sensitive(txt, log_remove_sensitive);
		rdr_debug_mask(reader, mask, "%s", txt);
	}
}

void rdr_ddump_mask(struct s_reader * reader, uint16_t mask, const uint8_t * buf, int n, char *fmt, ...) {
	if (config_WITH_DEBUG()) {
		char txt[2048];
		va_list args;
		va_start(args, fmt);
		vsnprintf(txt, sizeof(txt), fmt, args);
		va_end(args);
		cs_ddump_mask(mask, buf, n, "%s [%s] %s%s", reader->label, reader_desc_txt(reader), debug_mask_txt(mask), txt);
	}
}

/**
 * add one entitlement item to entitlements of reader.
 **/
void cs_add_entitlement(struct s_reader *rdr, uint16_t caid, uint32_t provid, uint64_t id, uint32_t class, time_t start, time_t end, uint8_t type)
{
	if (!rdr->ll_entitlements) rdr->ll_entitlements = ll_create("ll_entitlements");

	S_ENTITLEMENT *item;
	if(cs_malloc(&item,sizeof(S_ENTITLEMENT), -1)){

		// fill item
		item->caid = caid;
		item->provid = provid;
		item->id = id;
		item->class = class;
		item->start = start;
		item->end = end;
		item->type = type;

		//add item
		ll_append(rdr->ll_entitlements, item);

	  // cs_debug_mask(D_TRACE, "entitlement: Add caid %4X id %4X %s - %s ", item->caid, item->id, item->start, item->end);
	}

}

/**
 * clears entitlements of reader.
 **/
void cs_clear_entitlement(struct s_reader *rdr)
{
	if (!rdr->ll_entitlements)
		return;

	ll_clear_data(rdr->ll_entitlements);
}


void casc_check_dcw(struct s_reader * reader, int32_t idx, int32_t rc, uchar *cw)
{
	int32_t i, pending=0;
	time_t t = time(NULL);
	ECM_REQUEST *ecm;
	struct s_client *cl = reader->client;

	if(!cl) return;

	for (i = 0; i < cfg.max_pending; i++) {
		ecm = &cl->ecmtask[i];
		if ((ecm->rc>=10) && ecm->caid == cl->ecmtask[idx].caid && (!memcmp(ecm->ecmd5, cl->ecmtask[idx].ecmd5, CS_ECMSTORESIZE))) {
			if (ecm->parent) {
				if (rc) {
					//write_ecm_answer(reader, ecm->parent, (i==idx) ? E_FOUND : E_CACHE2, 0, cw, NULL); //Cache2 now generated by distribute_ecm
					write_ecm_answer(reader, ecm->parent, E_FOUND, 0, cw, NULL);
				} else {
					write_ecm_answer(reader, ecm->parent, E_NOTFOUND, 0 , NULL, NULL);
				}
			}
			ecm->idx=0;
			ecm->rc=0;
		}

		if (ecm->rc>=10 && (t-(uint32_t)ecm->tps.time > ((cfg.ctimeout + 500) / 1000) + 1)) { // drop timeouts
			ecm->rc=0;
#ifdef WITH_LB
			send_reader_stat(reader, ecm, NULL, E_TIMEOUT);
#endif
		}

		if (ecm->rc >= 10)
			pending++;
	}
	cl->pending=pending;
}

int32_t hostResolve(struct s_reader *rdr){
   struct s_client *cl = rdr->client;

   if(!cl) return 0;

   IN_ADDR_T last_ip;
   IP_ASSIGN(last_ip, cl->ip);
   cs_resolve(rdr->device, &cl->ip, &cl->udp_sa, &cl->udp_sa_len);
   IP_ASSIGN(SIN_GET_ADDR(cl->udp_sa), cl->ip);

   if (!IP_EQUAL(cl->ip, last_ip)) {
     cs_log("%s: resolved ip=%s", rdr->device, cs_inet_ntoa(cl->ip));
   }

   return IP_ISSET(cl->ip);
}

void clear_block_delay(struct s_reader *rdr) {
   rdr->tcp_block_delay = 0;
   cs_ftime(&rdr->tcp_block_connect_till);
}

void block_connect(struct s_reader *rdr) {
  if (!rdr->tcp_block_delay)
  	rdr->tcp_block_delay = 100; //starting blocking time, 100ms
  cs_ftime(&rdr->tcp_block_connect_till);
  rdr->tcp_block_connect_till.time += rdr->tcp_block_delay / 1000;
  rdr->tcp_block_connect_till.millitm += rdr->tcp_block_delay % 1000;
  rdr->tcp_block_delay *= 4; //increment timeouts
  if (rdr->tcp_block_delay >= 60*1000)
    rdr->tcp_block_delay = 60*1000; //max 1min, todo config
  rdr_debug_mask(rdr, D_TRACE, "tcp connect blocking delay set to %d", rdr->tcp_block_delay);
}

int32_t is_connect_blocked(struct s_reader *rdr) {
  struct timeb cur_time;
  cs_ftime(&cur_time);
  int32_t blocked = (rdr->tcp_block_delay && comp_timeb(&cur_time, &rdr->tcp_block_connect_till) < 0);
  if (blocked) {
		int32_t time = 1000*(rdr->tcp_block_connect_till.time-cur_time.time)
				+rdr->tcp_block_connect_till.millitm-cur_time.millitm;
		rdr_debug_mask(rdr, D_TRACE, "connection blocked, retrying in %ds", time/1000);
  }
  return blocked;
}

int32_t network_tcp_connection_open(struct s_reader *rdr)
{
	if (!rdr) return -1;
	struct s_client *client = rdr->client;
	struct sockaddr_in loc_sa;

	memset((char *)&client->udp_sa, 0, sizeof(client->udp_sa));

	IN_ADDR_T last_ip;
	IP_ASSIGN(last_ip, client->ip);
	if (!hostResolve(rdr))
		return -1;

	if (!IP_EQUAL(last_ip, client->ip)) //clean blocking delay on ip change:
		clear_block_delay(rdr);

	if (is_connect_blocked(rdr)) { //inside of blocking delay, do not connect!
		return -1;
	}

	if (client->reader->r_port<=0) {
		rdr_log(client->reader, "invalid port %d for server %s", client->reader->r_port, client->reader->device);
		return -1;
	}

	client->is_udp=(rdr->typ==R_CAMD35);

	rdr_log(rdr, "connecting to %s:%d", rdr->device, rdr->r_port);

	if (client->udp_fd)
		rdr_log(rdr, "WARNING: client->udp_fd was not 0");

	int s_domain = PF_INET;
#ifdef IPV6SUPPORT
	if (!IN6_IS_ADDR_V4MAPPED(&rdr->client->ip) && !IN6_IS_ADDR_V4COMPAT(&rdr->client->ip))
		s_domain = PF_INET6;
#endif
	int s_type   = client->is_udp ? SOCK_DGRAM : SOCK_STREAM;
	int s_proto  = client->is_udp ? IPPROTO_UDP : IPPROTO_TCP;

	if ((client->udp_fd = socket(s_domain, s_type, s_proto)) < 0) {
		rdr_log(rdr, "Socket creation failed (errno=%d %s)", errno, strerror(errno));
		client->udp_fd = 0;
		block_connect(rdr);
		return -1;
	}

	set_socket_priority(client->udp_fd, cfg.netprio);

	int32_t keep_alive = 1;
	setsockopt(client->udp_fd, SOL_SOCKET, SO_KEEPALIVE, (void *)&keep_alive, sizeof(keep_alive));

	int32_t flag = 1;
	setsockopt(client->udp_fd, IPPROTO_TCP, TCP_NODELAY, (void *)&flag, sizeof(flag));

	if (client->reader->l_port>0) {
		memset((char *)&loc_sa,0,sizeof(loc_sa));
		loc_sa.sin_family = AF_INET;
		if (IP_ISSET(cfg.srvip))
			IP_ASSIGN(SIN_GET_ADDR(loc_sa), cfg.srvip);
		else
			loc_sa.sin_addr.s_addr = INADDR_ANY;

		loc_sa.sin_port = htons(client->reader->l_port);
		if (bind(client->udp_fd, (struct sockaddr *)&loc_sa, sizeof (loc_sa))<0) {
			rdr_log(rdr, "bind failed (errno=%d %s)", errno, strerror(errno));
			close(client->udp_fd);
			client->udp_fd = 0;
			block_connect(rdr);
			return -1;
		}
	}

#ifdef IPV6SUPPORT
	if (IN6_IS_ADDR_V4MAPPED(&rdr->client->ip) || IN6_IS_ADDR_V4COMPAT(&rdr->client->ip)) {
		((struct sockaddr_in *)(&client->udp_sa))->sin_family = AF_INET;
		((struct sockaddr_in *)(&client->udp_sa))->sin_port = htons((uint16_t)client->reader->r_port);
	} else {
		((struct sockaddr_in6 *)(&client->udp_sa))->sin6_family = AF_INET6;
		((struct sockaddr_in6 *)(&client->udp_sa))->sin6_port = htons((uint16_t)client->reader->r_port);
	}
#else
	client->udp_sa.sin_family = AF_INET;
	client->udp_sa.sin_port = htons((uint16_t)client->reader->r_port);
#endif

	rdr_debug_mask(rdr, D_TRACE, "socket open for %s fd=%d", rdr->ph.desc, client->udp_fd);

	if (client->is_udp) {
		rdr->tcp_connected = 1;
		return client->udp_fd;
	}

       int32_t fl = fcntl(client->udp_fd, F_GETFL);
	fcntl(client->udp_fd, F_SETFL, O_NONBLOCK);

	int32_t res = connect(client->udp_fd, (struct sockaddr *)&client->udp_sa, client->udp_sa_len);
	if (res == -1) {
		int32_t r = -1;
		if (errno == EINPROGRESS || errno == EALREADY) {
			struct pollfd pfd;
			pfd.fd = client->udp_fd;
			pfd.events = POLLOUT;
			int32_t rc = poll(&pfd, 1, 3000);
			if (rc > 0) {
				uint32_t l = sizeof(r);
				if (getsockopt(client->udp_fd, SOL_SOCKET, SO_ERROR, &r, (socklen_t*)&l) != 0)
					r = -1;
				else
					errno = r;
			} else {
				errno = ETIMEDOUT;
			}
		}
		if (r != 0) {
			rdr_log(rdr, "connect failed: %s", strerror(errno));
			block_connect(rdr); //connect has failed. Block connect for a while
			close(client->udp_fd);
			client->udp_fd = 0;
			return -1;
		}
	}

	fcntl(client->udp_fd, F_SETFL, fl); //restore blocking mode

	setTCPTimeouts(client->udp_fd);
	clear_block_delay(rdr);
	client->last=client->login=time((time_t*)0);
	client->last_caid=client->last_srvid=0;
	client->pfd = client->udp_fd;
	rdr->tcp_connected = 1;
	rdr_debug_mask(rdr, D_TRACE, "connect succesfull fd=%d", client->udp_fd);
	return client->udp_fd;
}

void network_tcp_connection_close(struct s_reader *reader, char *reason)
{
	if (!reader) {
		//only proxy reader should call this, client connections are closed on thread cleanup
		cs_log("WARNING: invalid client");
		cs_disconnect_client(cur_client());
		return;
	}

	struct s_client *cl = reader->client;
	if(!cl) return;
	int32_t fd = cl->udp_fd;

	int32_t i;

	if (fd) {
		rdr_log(reader, "disconnected: reason %s", reason ? reason : "undef");
		close(fd);

		cl->udp_fd = 0;
		cl->pfd = 0;
	}

	reader->tcp_connected = 0;
	reader->card_status = UNKNOWN;

	if (cl->ecmtask) {
		for (i = 0; i < cfg.max_pending; i++) {
			cl->ecmtask[i].idx = 0;
			cl->ecmtask[i].rc = 0;
		}
	}
}

void casc_do_sock_log(struct s_reader * reader)
{
  int32_t i, idx;
  uint16_t caid, srvid;
  uint32_t provid;
  struct s_client *cl = reader->client;

  if(!cl) return;

  idx=reader->ph.c_recv_log(&caid, &provid, &srvid);
  cl->last=time((time_t*)0);
  if (idx<0) return;        // no dcw-msg received

  if(!cl->ecmtask) {
    rdr_log(reader, "WARNING: ecmtask not a available");
    return;
  }

  for (i = 0; i < cfg.max_pending; i++)
  {
    if (  (cl->ecmtask[i].rc>=10)
       && (cl->ecmtask[i].idx==idx)
       && (cl->ecmtask[i].caid==caid)
       && (cl->ecmtask[i].prid==provid)
       && (cl->ecmtask[i].srvid==srvid))
    {
      casc_check_dcw(reader, i, 0, cl->ecmtask[i].cw);  // send "not found"
      break;
    }
  }
}

int32_t casc_process_ecm(struct s_reader * reader, ECM_REQUEST *er)
{
	int32_t rc, n, i, sflag, pending=0;
	time_t t;//, tls;
	struct s_client *cl = reader->client;

	if(!cl || !cl->ecmtask) {
		rdr_log(reader, "WARNING: ecmtask not a available");
		return -1;
	}

	uchar buf[512];

	t=time((time_t *)0);
	ECM_REQUEST *ecm;
	for (i = 0; i < cfg.max_pending; i++) {
		ecm = &cl->ecmtask[i];
		if ((ecm->rc>=10) && (t-(uint32_t)ecm->tps.time > ((cfg.ctimeout + 500) / 1000) + 1)) { // drop timeouts
			ecm->rc=0;
#ifdef WITH_LB
			send_reader_stat(reader, ecm, NULL, E_TIMEOUT);
#endif
		}
	}

	for (n = -1, i = 0, sflag = 1; i < cfg.max_pending; i++) {
		ecm = &cl->ecmtask[i];
		if (n<0 && (ecm->rc<10))   // free slot found
			n=i;

		// ecm already pending
		// ... this level at least
		if ((ecm->rc>=10) &&  er->caid == ecm->caid && (!memcmp(er->ecmd5, ecm->ecmd5, CS_ECMSTORESIZE)) && (er->level<=ecm->level))
			sflag=0;

		if (ecm->rc >=10)
			pending++;
	}
	cl->pending=pending;

	if (n<0) {
		rdr_log(reader, "WARNING: reader ecm pending table overflow !!");
		return(-2);
	}

	memcpy(&cl->ecmtask[n], er, sizeof(ECM_REQUEST));
	cl->ecmtask[n].matching_rdr = NULL; //This avoids double free of matching_rdr!
#ifdef CS_CACHEEX
	cl->ecmtask[n].csp_lastnodes = NULL; //This avoids double free of csp_lastnodes!
#endif
	cl->ecmtask[n].parent = er;

	if( reader->typ == R_NEWCAMD )
		cl->ecmtask[n].idx=(cl->ncd_msgid==0)?2:cl->ncd_msgid+1;
	else {
		if (!cl->idx)
    			cl->idx = 1;
		cl->ecmtask[n].idx=cl->idx++;
	}

	cl->ecmtask[n].rc=10;
	cs_debug_mask(D_TRACE, "---- ecm_task %d, idx %d, sflag=%d, level=%d", n, cl->ecmtask[n].idx, sflag, er->level);

	cs_ddump_mask(D_ATR, er->ecm, er->l, "casc ecm:");
	rc=0;
	if (sflag) {
		if ((rc=reader->ph.c_send_ecm(cl, &cl->ecmtask[n], buf)))
			casc_check_dcw(reader, n, 0, cl->ecmtask[n].cw);  // simulate "not found"
		else
			cl->last_idx = cl->ecmtask[n].idx;
		reader->last_s = t;   // used for inactive_timeout and reconnect_timeout in TCP reader
	}

	if (cl->idx>0x1ffe) cl->idx=1;

	return(rc);
}

static int32_t reader_store_emm(uchar type, uchar *emmd5)
{
  int32_t rc;
  struct s_client *cl = cur_client();
  memcpy(cl->emmcache[cl->rotate].emmd5, emmd5, CS_EMMSTORESIZE);
  cl->emmcache[cl->rotate].type=type;
  cl->emmcache[cl->rotate].count=1;
//  cs_debug_mask(D_READER, "EMM stored (index %d)", rotate);
  rc=cl->rotate;
  cl->rotate=(++cl->rotate < CS_EMMCACHESIZE)?cl->rotate:0;
  return(rc);
}

void reader_get_ecm(struct s_reader * reader, ECM_REQUEST *er)
{
	struct s_client *cl = reader->client;
	if(!cl) return;
	if (er->rc<=E_STOPPED) {
		//TODO: not sure what this is for, but it was in mpcs too.
		// ecm request was already answered when the request was started (this ECM_REQUEST is a copy of client->ecmtask[] ECM_REQUEST).
		// send_dcw is a client function but reader_get_ecm is only called from reader functions where client->ctyp is not set and so send_dcw() will segfault.
		// so we could use send_dcw(er->client, er) or write_ecm_answer(reader, er), but send_dcw wont be threadsafe from here cause there may be multiple threads accessing same s_client struct.
		// maybe rc should be checked before request is sent to reader but i could not find the reason why this is happening now and not in v1.10 (zetack)
		//send_dcw(cl, er);
		rdr_debug_mask(reader, D_TRACE, "skip ecm %04X, rc=%d", er->checksum, er->rc);
		return;
	}

	if (!chk_bcaid(er, &reader->ctab)) {
		rdr_debug_mask(reader, D_READER, "caid %04X filtered", er->caid);
		write_ecm_answer(reader, er, E_NOTFOUND, E2_CAID, NULL, NULL);
		return;
	}

	// cache2
	struct ecm_request_t *ecm = check_cwcache(er, cl);
	if (ecm && ecm->rc <= E_NOTFOUND) {
		rdr_debug_mask(reader, D_TRACE, "ecm %04X answer from cache", er->checksum);
		write_ecm_answer(reader, er, E_CACHE2, 0, ecm->cw, NULL);
		return;
	}

	if (is_cascading_reader(reader)) {
		cl->last_srvid=er->srvid;
		cl->last_caid=er->caid;
		casc_process_ecm(reader, er);
		cl->lastecm=time((time_t*)0);
		return;
	}

#ifdef WITH_CARDREADER

	cs_ddump_mask(D_ATR, er->ecm, er->l, "ecm:");

	struct timeb tps, tpe;
	cs_ftime(&tps);

	struct s_ecm_answer ea;
	memset(&ea, 0, sizeof(struct s_ecm_answer));

	int32_t rc = reader_ecm(reader, er, &ea);

	ea.rc = E_FOUND; //default assume found
	ea.rcEx = 0; //no special flag

	if(rc == ERROR ){
		char buf[32];
		rdr_debug_mask(reader, D_TRACE, "Error processing ecm for caid %04X, srvid %04X, servicename: %s",
			er->caid, er->srvid, get_servicename(cl, er->srvid, er->caid, buf));
		ea.rc = E_NOTFOUND;
		ea.rcEx = 0;
		if (reader->typ == R_SC8in1 && reader->sc8in1_config->mcr_type) {
			char text[] = {'S', (char)reader->slot+0x30, 'E', 'e', 'r'};
			MCR_DisplayText(reader, text, 5, 400, 0);
		}
	}

	if(rc == E_CORRUPT ){
		char buf[32];
		rdr_debug_mask(reader, D_TRACE, "Error processing ecm for caid %04X, srvid %04X, servicename: %s",
			er->caid, er->srvid, get_servicename(cl, er->srvid, er->caid, buf));
		ea.rc = E_NOTFOUND;
		ea.rcEx = E2_WRONG_CHKSUM; //flag it as wrong checksum
		memcpy (ea.msglog,"Invalid ecm type for card",25);
	}
	cs_ftime(&tpe);
	cl->lastecm=time((time_t*)0);

	rdr_debug_mask(reader, D_TRACE, "ecm: %04X real time: %ld ms",
		htons(er->checksum), 1000 * (tpe.time - tps.time) + tpe.millitm - tps.millitm);

	write_ecm_answer(reader, er, ea.rc, ea.rcEx, ea.cw, ea.msglog);
	reader_post_process(reader);
#endif
}

void reader_log_emm(struct s_reader * reader, EMM_PACKET *ep, int32_t i, int32_t rc, struct timeb *tps) {
	char *rtxt[] = { "error",
			is_cascading_reader(reader) ? "sent" : "written", "skipped",
			"blocked" };
	char *typedesc[] = { "unknown", "unique", "shared", "global" };
	struct s_client *cl = reader->client;
	struct timeb tpe;

	if (reader->logemm & (1 << rc)) {
		cs_ftime(&tpe);
		if (!tps)
			tps = &tpe;

		rdr_log(reader, "%s emmtype=%s, len=%d, idx=%d, cnt=%d: %s (%ld ms)",
				username(ep->client), typedesc[cl->emmcache[i].type], ep->emm[2],
				i, cl->emmcache[i].count, rtxt[rc],
				1000 * (tpe.time - tps->time) + tpe.millitm - tps->millitm);
	}

	if (rc) {
		cl->lastemm = time((time_t*) 0);
		led_status_emm_ok();
	}

#if defined(WEBIF) || defined(LCDSUPPORT)
	//counting results
	switch (rc) {
	case 0:
		reader->emmerror[ep->type]++;
		break;
	case 1:
		reader->emmwritten[ep->type]++;
		break;
	case 2:
		reader->emmskipped[ep->type]++;
		break;
	case 3:
		reader->emmblocked[ep->type]++;
		break;
	}
#endif
}

int32_t reader_do_emm(struct s_reader * reader, EMM_PACKET *ep)
{
  int32_t i, rc, ecs;
  unsigned char md5tmp[MD5_DIGEST_LENGTH];
  struct timeb tps;
  struct s_client *cl = reader->client;

  if(!cl) return 0;

    cs_ftime(&tps);

	MD5(ep->emm, ep->emm[2], md5tmp);

        for (i=ecs=0; (i<CS_EMMCACHESIZE) ; i++) {
       	if (!memcmp(cl->emmcache[i].emmd5, md5tmp, CS_EMMSTORESIZE)) {
			cl->emmcache[i].count++;
			if (reader->cachemm){
				if (cl->emmcache[i].count > reader->rewritemm){
					ecs=2; //skip emm
				}
				else
					ecs=1; //rewrite emm
			}
		break;
		}
	}

	//Ecs=0 not found in cache
	//Ecs=1 found in cache, rewrite emm
	//Ecs=2 skip

  if ((rc=ecs)<2)
  {
          if (is_cascading_reader(reader)) {
                  rdr_debug_mask(reader, D_READER, "network emm reader");

                  if (reader->ph.c_send_emm) {
                          rc=reader->ph.c_send_emm(ep);
                  } else {
                          rdr_debug_mask(reader, D_READER, "send_emm() support missing");
                          rc=0;
                  }
          } else {
                  rdr_debug_mask(reader, D_READER, "local emm reader");
#ifdef WITH_CARDREADER
                  rc=reader_emm(reader, ep);
#else
                  rc=0;
#endif
          }

          if (!ecs)
        	  i=reader_store_emm(ep->type, md5tmp);
  }

  reader_log_emm(reader, ep, i, rc, &tps);

  return(rc);
}

void reader_do_card_info(struct s_reader * reader)
{
#ifdef WITH_CARDREADER
      reader_card_info(reader);
#endif
      if (reader->ph.c_card_info)
      	reader->ph.c_card_info();
}

void reader_do_idle(struct s_reader * reader)
{
	if (reader->ph.c_idle)
		reader->ph.c_idle();
	else {
		time_t now;
		int32_t time_diff;
		time(&now);
		time_diff = abs(now - reader->last_s);
		if (time_diff>(reader->tcp_ito*60)) {
			struct s_client *cl = reader->client;
			if (cl && reader->tcp_connected && reader->ph.type==MOD_CONN_TCP) {
				cs_debug_mask(D_READER, "%s inactive_timeout, close connection (fd=%d)", reader->ph.desc, cl->pfd);
				network_tcp_connection_close(reader, "inactivity");
			} else
				reader->last_s = now;
		}
	}
}

int32_t reader_init(struct s_reader *reader) {
	struct s_client *client = reader->client;

	if (is_cascading_reader(reader)) {
		client->typ='p';
		client->port=reader->r_port;
		set_null_ip(&client->ip);

		if (!(reader->ph.c_init)) {
			rdr_log(reader, "FATAL: %s-protocol not supporting cascading", reader->ph.desc);
			return 0;
		}

		if (reader->ph.c_init(client)) {
			//proxy reader start failed
			return 0;
		}

		if ((reader->log_port) && (reader->ph.c_init_log))
			reader->ph.c_init_log();

		cs_malloc(&client->ecmtask, cfg.max_pending * sizeof(ECM_REQUEST), 1);

		rdr_log(reader, "proxy initialized, server %s:%d", reader->device, reader->r_port);
	}
#ifdef WITH_CARDREADER
	else {
		client->typ='r';
		set_localhost_ip(&client->ip);
		while (reader_device_init(reader)==2){
			int8_t i = 0;
			do{
				cs_sleepms(2000);
				if(!ll_contains(configured_readers, reader) || !check_client(client) || reader->enable != 1) return 0;
				++i;
			} while (i < 30);
		}
		if (reader->mhz > 2000) {
			rdr_log(reader, "Reader initialized (device=%s, detect=%s%s, pll max=%.2f Mhz, wanted cardmhz=%.2f Mhz",
				reader->device,
				reader->detect & 0x80 ? "!" : "",
				RDR_CD_TXT[reader->detect & 0x7f],
				(float)reader->mhz /100,
				(float)reader->cardmhz / 100);
		} else {
			rdr_log(reader, "Reader initialized (device=%s, detect=%s%s, mhz=%d, cardmhz=%d)",
				reader->device,
				reader->detect & 0x80 ? "!" : "",
				RDR_CD_TXT[reader->detect&0x7f],
				reader->mhz,
				reader->cardmhz);
		}
	}

#endif

	cs_malloc(&client->emmcache,CS_EMMCACHESIZE*(sizeof(struct s_emm)), 1);

	client->login=time((time_t*)0);
	client->init_done=1;

	return 1;
}

#if !defined(WITH_CARDREADER) && defined(WITH_STAPI)
/* Dummy function stub for stapi compiles without cardreader as libstapi needs it. */
int32_t ATR_InitFromArray (ATR * atr, const BYTE atr_buffer[ATR_MAX_SIZE], uint32_t length){
	return 0;
}
#endif
