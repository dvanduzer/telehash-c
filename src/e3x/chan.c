#include <string.h>
#include <stdlib.h>
#include "util.h"
#include "e3x.h"

e3chan_t e3chan_new(lob_t open){return NULL;} // open must be e3chan_receive or e3chan_send next yet
void e3chan_free(e3chan_t c){};
void e3chan_ev(e3chan_t c, e3ev_t ev){}; // timers only work with this set

// incoming packets
uint8_t e3chan_receive(e3chan_t c, lob_t inner){return 0;}; // usually sets/updates event timer, ret if accepted/valid into receiving queue
void e3chan_sync(e3chan_t c, uint8_t sync){}; // false to force start timers (any new handshake), true to cancel and resend last packet (after any e3x_sync)
lob_t e3chan_receiving(e3chan_t c){return NULL;}; // get next avail packet in order, null if nothing

// outgoing packets
lob_t e3chan_packet(e3chan_t c){return NULL;};  // creates a packet w/ necessary json, just a convenience
uint8_t e3chan_send(e3chan_t c, lob_t inner){return 0;}; // adds to sending queue, adds json if needed
lob_t e3chan_sending(e3chan_t c){return 0;}; // must be called after every send or receive, pass pkt to e3x_encrypt before sending

// convenience functions
uint32_t e3chan_id(e3chan_t c){return 0;}; // numeric of the open->cid
lob_t e3chan_open(e3chan_t c){return NULL;}; // returns the open packet (always cached)
uint8_t e3chan_state(e3chan_t c){return 0;}; // E3CHAN_OPENING, E3CHAN_OPEN, or E3CHAN_ENDED
uint32_t e3chan_size(e3chan_t c){return 0;}; // size (in bytes) of buffered data in or out

/*
// flags channel as ended either in or out
void doend(e3chan_t c)
{
  if(!c || c->ended) return;
  DEBUG_PRINTF("channel ended %d",c->id);
  c->ended = 1;
}

// immediately removes channel, creates/sends packet to app to notify
void doerror(e3chan_t c, lob_t p, char *err)
{
  if(c->ended) return;
  DEBUG_PRINTF("channel fail %d %s %d %d %d",c->id,err?err:lob_get_str(p,"err"),c->timeout,c->tsent,c->trecv);

  // unregister for any more packets immediately
  xht_set(c->to->chans,(char*)c->hexid,NULL);
  if(c->id > c->to->chanMax) c->to->chanMax = c->id;

  // convenience to generate error packet
  if(!p)
  {
    if(!(p = lob_new())) return;
    lob_set_str(p,"err",err?err:"unknown");
    lob_set_int(p,"c",c->id);
  }

  // send error to app immediately, will doend() when processed
  p->next = c->in;
  c->in = p;
  e3chan_queue(c);
}

void e3chan_free(e3chan_t c)
{
  lob_t p;
  if(!c) return;

  // if there's an arg set, we can't free and must notify channel handler
  if(c->arg) return e3chan_queue(c);

  DEBUG_PRINTF("channel free %d",c->id);

  // unregister for packets (if registered)
  if(xht_get(c->to->chans,(char*)c->hexid) == (void*)c)
  {
    xht_set(c->to->chans,(char*)c->hexid,NULL);
    // to prevent replay of this old id
    if(c->id > c->to->chanMax) c->to->chanMax = c->id;
  }
  xht_set(c->s->index,(char*)c->uid,NULL);

  // remove references
  e3chan_dequeue(c);
  if(c->reliable)
  {
    e3chan_seq_free(c);
    e3chan_miss_free(c);
  }
  while(c->in)
  {
    DEBUG_PRINTF("unused packets on channel %d",c->id);
    p = c->in;
    c->in = p->next;
    lob_free(p);
  }
  while(c->notes)
  {
    DEBUG_PRINTF("unused notes on channel %d",c->id);
    p = c->notes;
    c->notes = p->next;
    lob_free(p);
  }
  // TODO, if it's the last channel, bucket_rem(c->s->active, c);
  free(c->type);
  free(c);
}

void walkreset(xht_t h, const char *key, void *val, void *arg)
{
  e3chan_t c = (e3chan_t)val;
  if(!c) return;
  if(c->opened) return doerror(c,NULL,"reset");
  // flush any waiting packets
  if(c->reliable) e3chan_miss_resend(c);
  else switch_send(c->s,lob_copy(c->in));
}
void e3chan_reset(switch_t s, hashname_t to)
{
  // fail any existing open channels
  xht_walk(to->chans, &walkreset, NULL);
  // reset max id tracking
  to->chanMax = 0;
}

void walktick(xht_t h, const char *key, void *val, void *arg)
{
  uint32_t last;
  e3chan_t c = (e3chan_t)val;

  // latent cleanup/free
  if(c->ended && (c->tfree++) > c->timeout) return e3chan_free(c);

  // any channel can get tick event
  if(c->tick) c->tick(c);

  // these are administrative/internal channels (to our own hashname)
  if(c->to == c->s->id) return;

  // do any auto-resend packet timers
  if(c->tresend)
  {
    // TODO check packet resend timers, e3chan_miss_resend or c->in
  }

  // unreliable timeout, use last received unless none, then last sent
  if(!c->reliable)
  {
    last = (c->trecv) ? c->trecv : c->tsent;
    if(c->timeout && (c->s->tick - last) > c->timeout) doerror(c,NULL,"timeout");
  }

}

void e3chan_tick(switch_t s, hashname_t hn)
{
  xht_walk(hn->chans, &walktick, NULL);
}

e3chan_t e3chan_reliable(e3chan_t c, int window)
{
  if(!c || !window || c->tsent || c->trecv || c->reliable) return c;
  c->reliable = window;
  e3chan_seq_init(c);
  e3chan_miss_init(c);
  return c;
}

// kind of a macro, just make a reliable channel of this type
e3chan_t e3chan_start(switch_t s, char *hn, char *type)
{
  e3chan_t c;
  if(!s || !hn) return NULL;
  c = e3chan_new(s, hashname_gethex(s->index,hn), type, 0);
  return e3chan_reliable(c, s->window);
}

e3chan_t e3chan_new(switch_t s, hashname_t to, char *type, uint32_t id)
{
  e3chan_t c;
  if(!s || !to || !type) return NULL;

  // use new id if none given
  if(!to->chanOut) to->chanOut = (strncmp(s->id->hexname,to->hexname,64) > 0) ? 1 : 2;
  if(!id)
  {
    id = to->chanOut;
    to->chanOut += 2;
  }else{
    // externally given id can't be ours
    if(id % 2 == to->chanOut % 2) return NULL;
    // must be a newer id
    if(id <= to->chanMax) return NULL;
  }

  DEBUG_PRINTF("channel new %d %s %s",id,type,to->hexname);
  c = malloc(sizeof (struct e3chan_struct));
  memset(c,0,sizeof (struct e3chan_struct));
  c->type = malloc(strlen(type)+1);
  memcpy(c->type,type,strlen(type)+1);
  c->s = s;
  c->to = to;
  c->timeout = CHAN_TIMEOUT;
  c->id = id;
  util_hex((unsigned char*)&(s->uid),4,(unsigned char*)c->uid); // switch-wide unique id
  s->uid++;
  util_hex((unsigned char*)&(c->id),4,(unsigned char*)c->hexid);
  // first one on this hashname
  if(!to->chans)
  {
    to->chans = xht_new(17);
    bucket_add(s->active, to);
  }
  xht_set(to->chans,(char*)c->hexid,c);
  xht_set(s->index,(char*)c->uid,c);
  return c;
}

void e3chan_timeout(e3chan_t c, uint32_t seconds)
{
  if(!c) return;
  c->timeout = seconds;
}

e3chan_t e3chan_in(switch_t s, hashname_t from, lob_t p)
{
  e3chan_t c;
  unsigned long id;
  char hexid[9];
  if(!from || !p) return NULL;

  id = strtol(lob_get_str(p,"c"), NULL, 10);
  util_hex((unsigned char*)&id,4,(unsigned char*)hexid);
  c = xht_get(from->chans, hexid);
  if(c) return c;

  return e3chan_new(s, from, lob_get_str(p, "type"), id);
}

// get the next incoming note waiting to be handled
lob_t e3chan_notes(e3chan_t c)
{
  lob_t note;
  if(!c) return NULL;
  note = c->notes;
  if(note) c->notes = note->next;
  return note;
}

// create a new note tied to this channel
lob_t e3chan_note(e3chan_t c, lob_t note)
{
  if(!note) note = lob_new();
  lob_set_str(note,".from",(char*)c->uid);
  return note;
}

// send this note back to the sender
int e3chan_reply(e3chan_t c, lob_t note)
{
  char *from;
  if(!c || !(from = lob_get_str(note,".from"))) return -1;
  lob_set_str(note,".to",from);
  lob_set_str(note,".from",(char*)c->uid);
  return switch_note(c->s,note);
}

// create a packet ready to be sent for this channel
lob_t e3chan_packet(e3chan_t c)
{
  lob_t p;
  if(!c) return NULL;
  p = c->reliable?e3chan_seq_packet(c):lob_new();
  if(!p) return NULL;
  p->to = c->to;
  if(!c->tsent && !c->trecv)
  {
    lob_set_str(p,"type",c->type);
  }
  lob_set_int(p,"c",c->id);
  return p;
}

lob_t e3chan_pop(e3chan_t c)
{
  lob_t p;
  if(!c) return NULL;
  // this allows errors to be processed immediately
  if((p = c->in))
  {
    c->in = p->next;
    if(!c->in) c->inend = NULL;    
  }
  if(!p && c->reliable) p = e3chan_seq_pop(c);

  // change state as soon as an err/end is processed
  if(lob_get_str(p,"err") || util_cmp(lob_get_str(p,"end"),"true") == 0) doend(c);
  return p;
}

// add to processing queue
void e3chan_queue(e3chan_t c)
{
  e3chan_t step;
  // add to switch queue
  step = c->s->chans;
  if(c->next || step == c) return;
  while(step && (step = step->next)) if(step == c) return;
  c->next = c->s->chans;
  c->s->chans = c;
}

// remove from processing queue
void e3chan_dequeue(e3chan_t c)
{
  e3chan_t step = c->s->chans;
  if(step == c)
  {
    c->s->chans = c->next;
    c->next = NULL;
    return;
  }
  step = c->s->chans;
  while(step) step = (step->next == c) ? c->next : step->next;
  c->next = NULL;
}

// internal, receives/processes incoming packet
void e3chan_receive(e3chan_t c, lob_t p)
{
  if(!c || !p) return;
  DEBUG_PRINTF("channel in %d %d %.*s",c->id,p->body_len,p->json_len,p->json);
  c->trecv = c->s->tick;
  DEBUG_PRINTF("recv %d",c);
  
  // errors are immediate
  if(lob_get_str(p,"err")) return doerror(c,p,NULL);

  if(!c->opened)
  {
    DEBUG_PRINTF("channel opened %d",c->id);
    // remove any cached outgoing packet
    lob_free(c->in);
    c->in = NULL;
    c->opened = 1;
  }

  // TODO, only queue so many packets if we haven't responded ever (to limit ignored unsupported channels)
  if(c->reliable)
  {
    e3chan_miss_check(c,p);
    if(!e3chan_seq_receive(c,p)) return; // queued, nothing to do
  }else{
    // add to the end of the raw packet queue
    if(c->inend)
    {
      c->inend->next = p;
      c->inend = p;
      return;
    }
    c->inend = c->in = p;    
  }

  // queue for processing
  e3chan_queue(c);
}

// convenience
void e3chan_end(e3chan_t c, lob_t p)
{
  if(!c) return;
  if(!p && !(p = e3chan_packet(c))) return;
  lob_set(p,"end","true",4);
  e3chan_send(c,p);
}

// send errors directly
e3chan_t e3chan_fail(e3chan_t c, char *err)
{
  lob_t p;
  if(!c) return NULL;
  if(!(p = lob_new())) return c;
  doend(c);
  p->to = c->to;
  lob_set_str(p,"err",err?err:"unknown");
  lob_set_int(p,"c",c->id);
  switch_send(c->s,p);
  return c;
}

// smartly send based on what type of channel we are
void e3chan_send(e3chan_t c, lob_t p)
{
  if(!p) return;
  if(!c) return (void)lob_free(p);
  DEBUG_PRINTF("channel out %d %.*s",c->id,p->json_len,p->json);
  c->tsent = c->s->tick;
  if(c->reliable && lob_get_str(p,"seq")) p = lob_copy(p); // miss tracks the original p = e3chan_packet(), a sad hack
  if(lob_get_str(p,"err") || util_cmp(lob_get_str(p,"end"),"true") == 0) doend(c); // track sending error/end
  else if(!c->reliable && !c->opened && !c->in) c->in = lob_copy(p); // if normal unreliable start packet, track for resends
  
  switch_send(c->s,p);
}

// optionally sends reliable channel ack-only if needed
void e3chan_ack(e3chan_t c)
{
  lob_t p;
  if(!c || !c->reliable) return;
  p = e3chan_seq_ack(c,NULL);
  if(!p) return;
  DEBUG_PRINTF("channel ack %d %.*s",c->id,p->json_len,p->json);
  switch_send(c->s,p);
}
*/