// -*- c-basic-offset: 4 -*-
/*
 * toipflowdumps.{cc,hh} -- creates separate trace files for each flow
 * Eddie Kohler
 *
 * Copyright (c) 2002 International Computer Science Institute
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, subject to the conditions
 * listed in the Click LICENSE file. These conditions include: you must
 * preserve this copyright notice, and you cannot mention the copyright
 * holders in advertising related to the Software without their permission.
 * The Software is provided WITHOUT ANY WARRANTY, EXPRESS OR IMPLIED. This
 * notice is a summary of the Click LICENSE file; the license in that file is
 * legally binding.
 */

#include <click/config.h>
#include "aggpktcounter.hh"
#include <click/confparse.hh>
#include <click/error.hh>
#include <click/packet_anno.hh>
#include <click/router.hh>
#include <click/straccum.hh>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
CLICK_DECLS

AggregatePacketCounter::Flow::Flow(uint32_t aggregate, int columns)
    : _aggregate(aggregate), _next(0), _counts(new Vector<uint32_t>[columns])
{
}

void
AggregatePacketCounter::Flow::add(uint32_t packetno, int column)
{
    if (_counts[column].size() <= (int) packetno)
	_counts[column].resize(packetno + 1, 0);
    _counts[column].at_u(packetno)++;
}

AggregatePacketCounter::packetctr_t
AggregatePacketCounter::Flow::column_count(int column) const
{
    packetctr_t count = 0;
    for (const uint32_t *v = _counts[column].begin(); v < _counts[column].end(); v++)
	count += *v;
    return count;
}

void
AggregatePacketCounter::Flow::undelivered(Vector<uint32_t> &undelivered) const
{
    int packetno;
    int min_n = (_counts[0].size() < _counts[1].size() ? _counts[0].size() : _counts[1].size());
    for (packetno = 0; packetno < min_n; packetno++)
	if (_counts[0].at_u(packetno) > _counts[1].at_u(packetno))
	    undelivered.push_back(packetno);
    for (; packetno < _counts[0].size(); packetno++)
	if (_counts[0].at_u(packetno))
	    undelivered.push_back(packetno);
}


AggregatePacketCounter::AggregatePacketCounter()
{
    MOD_INC_USE_COUNT;
    for (int i = 0; i < NFLOWMAP; i++)
	_flowmap[i] = 0;
}

AggregatePacketCounter::~AggregatePacketCounter()
{
    MOD_DEC_USE_COUNT;
}

void
AggregatePacketCounter::notify_ninputs(int n)
{
    set_ninputs(n < 1 ? 1 : n);
    set_noutputs(n < 1 ? 1 : n);
}

int
AggregatePacketCounter::configure(Vector<String> &conf, ErrorHandler *errh)
{
    Element *e = 0;
    _packetno = 0;
    
    if (cp_va_parse(conf, this, errh,
		    cpKeywords,
		    "NOTIFIER", cpElement, "aggregate deletion notifier", &e,
		    "PACKETNO", cpInteger, "packet number annotation (-1..1)", &_packetno,
		    0) < 0)
	return -1;

    if (_packetno > 1)
	return errh->error("'PACKETNO' cannot be bigger than 1");
    /*if (e && !(_agg_notifier = (AggregateNotifier *)e->cast("AggregateNotifier")))
      return errh->error("%s is not an AggregateNotifier", e->id().cc()); */

    return 0;
}

int
AggregatePacketCounter::initialize(ErrorHandler *)
{
    _total_flows = _total_packets = 0;
    //if (_agg_notifier)
    //_agg_notifier->add_listener(this);
    //_gc_timer.initialize(this);
    return 0;
}

void
AggregatePacketCounter::end_flow(Flow *f, ErrorHandler *)
{
    /*    if (f->npackets() >= _mincount) {
	f->output(errh);
	if (_gzip && f->filename() != "-")
	    if (add_compressable(f->filename(), errh) < 0)
		_gzip = false;
    } else
    f->unlink(errh);*/
    delete f;
}

void
AggregatePacketCounter::cleanup(CleanupStage)
{
    ErrorHandler *errh = ErrorHandler::default_handler();
    for (int i = 0; i < NFLOWMAP; i++)
	while (Flow *f = _flowmap[i]) {
	    _flowmap[i] = f->next();
	    end_flow(f, errh);
	}
    if (_total_packets > 0 && _total_flows == 0)
	errh->lwarning(declaration(), "saw no packets with aggregate annotations");
}

AggregatePacketCounter::Flow *
AggregatePacketCounter::find_flow(uint32_t agg)
{
    if (agg == 0)
	return 0;
    
    int bucket = (agg & (NFLOWMAP - 1));
    Flow *prev = 0, *f = _flowmap[bucket];
    while (f && f->aggregate() != agg) {
	prev = f;
	f = f->next();
    }

    if (f)
	/* nada */;
    else if ((f = new Flow(agg, ninputs()))) {
	prev = f;
	_total_flows++;
    } else
	return 0;

    if (prev) {
	prev->set_next(f->next());
	f->set_next(_flowmap[bucket]);
	_flowmap[bucket] = f;
    }
    
    return f;
}

inline void
AggregatePacketCounter::smaction(int port, Packet *p)
{
    _total_packets++;
    if (Flow *f = find_flow(AGGREGATE_ANNO(p))) {
	if (_packetno >= 0)
	    f->add(PACKET_NUMBER_ANNO(p, _packetno), port);
	else
	    f->add(0, port);
    }
}

void
AggregatePacketCounter::push(int port, Packet *p)
{
    smaction(port, p);
    output(port).push(p);
}

Packet *
AggregatePacketCounter::pull(int port)
{
    if (Packet *p = input(port).pull()) {
	smaction(port, p);
	return p;
    } else
	return 0;
}

/*
void
AggregatePacketCounter::aggregate_notify(uint32_t agg, AggregateEvent event, const Packet *)
{
    if (event == DELETE_AGG && find_aggregate(agg, 0)) {
	_gc_aggs.push_back(agg);
	_gc_aggs.push_back(click_jiffies());
	if (!_gc_timer.scheduled())
	    _gc_timer.schedule_after_ms(250);
    }
}

void
AggregatePacketCounter::gc_hook(Timer *t, void *thunk)
{
    AggregatePacketCounter *td = static_cast<AggregatePacketCounter *>(thunk);
    uint32_t limit_jiff = click_jiffies() - (CLICK_HZ / 4);
    int i;
    for (i = 0; i < td->_gc_aggs.size() && SEQ_LEQ(td->_gc_aggs[i+1], limit_jiff); i += 2)
	if (Flow *f = td->find_aggregate(td->_gc_aggs[i], 0)) {
	    int bucket = (f->aggregate() & (NFLOWMAP - 1));
	    assert(td->_flowmap[bucket] == f);
	    td->_flowmap[bucket] = f->next();
	    td->end_flow(f, ErrorHandler::default_handler());
	}
    if (i < td->_gc_aggs.size()) {
	td->_gc_aggs.erase(td->_gc_aggs.begin(), td->_gc_aggs.begin() + i);
	t->schedule_after_ms(250);
    }
}
*/

enum { H_CLEAR, H_COUNT };

String
AggregatePacketCounter::read_handler(Element *e, void *thunk)
{
    AggregatePacketCounter *ac = static_cast<AggregatePacketCounter *>(e);
    switch ((intptr_t)thunk) {
      case H_COUNT: {
	  packetctr_t count = 0;
	  for (int i = 0; i < NFLOWMAP; i++)
	      for (const Flow *f = ac->_flowmap[i]; f; f = f->next())
		  for (int col = 0; col < ac->ninputs(); col++)
		      count += f->column_count(col);
	  return String(count) + "\n";
      }
      default:
	return "<error>\n";
    }
}

int
AggregatePacketCounter::write_handler(const String &, Element *e, void *thunk, ErrorHandler *errh)
{
    AggregatePacketCounter *td = static_cast<AggregatePacketCounter *>(e);
    switch ((intptr_t)thunk) {
      case H_CLEAR:
	for (int i = 0; i < NFLOWMAP; i++)
	    while (Flow *f = td->_flowmap[i]) {
		td->_flowmap[i] = f->next();
		td->end_flow(f, errh);
	    }
	return 0;
      default:
	return -1;
    }
}

String
AggregatePacketCounter::undelivered_read_handler(Element *e, void *thunk)
{
    AggregatePacketCounter *ac = static_cast<AggregatePacketCounter *>(e);
    uint32_t aggregate = (uintptr_t)thunk;
    Vector<uint32_t> undelivered;
    if (Flow *f = ac->find_flow(aggregate))
	f->undelivered(undelivered);
    StringAccum sa;
    for (int i = 0; i < undelivered.size(); i++)
	sa << undelivered[i] << '\n';
    return sa.take_string();
}

int
AggregatePacketCounter::star_write_handler(const String &s, Element *e, void *, ErrorHandler *)
{
    uint32_t aggregate;
    if (s.substring(0, 11) == "undelivered" && cp_unsigned(s.substring(11), &aggregate) && e->ninputs() > 1) {
	e->add_read_handler(s, undelivered_read_handler, (void *)((uintptr_t)aggregate));
	return 0;
    } else
	return -1;
}

void
AggregatePacketCounter::add_handlers()
{
    add_write_handler("clear", write_handler, (void *)H_CLEAR);
    add_read_handler("count", read_handler, (void *)H_COUNT);
    add_write_handler("*", star_write_handler, 0);
}

CLICK_ENDDECLS
ELEMENT_REQUIRES(userlevel)
EXPORT_ELEMENT(AggregatePacketCounter)