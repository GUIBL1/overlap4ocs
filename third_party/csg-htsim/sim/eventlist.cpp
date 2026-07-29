// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-        

#include "eventlist.h"
#include "trigger.h"

#include <limits>

simtime_picosec EventList::_endtime = 0;
simtime_picosec EventList::_lasteventtime = 0;
EventList::pendingsources_t EventList::_pendingsources;
deque<TriggerTarget*> EventList::_pending_triggers;
uint64_t EventList::_next_insertion_order = 0;
uint64_t EventList::_processed_event_count = 0;
int EventList::_instanceCount = 0;
EventList* EventList::_theEventList = nullptr;

EventList::EventList()
{
    if (EventList::_instanceCount != 0) 
    {
        std::cerr << "There should be only one instance of EventList. Abort." << std::endl;
        abort();
    }

    EventList::_theEventList = this;
    EventList::_instanceCount += 1;
}

EventList& 
EventList::getTheEventList()
{
    if (EventList::_theEventList == nullptr) 
    {
        EventList::_theEventList = new EventList();
    }
    return *EventList::_theEventList;
}

void
EventList::setEndtime(simtime_picosec endtime)
{
    EventList::_endtime = endtime;
}

bool
EventList::doNextEvent()
{
    // Triggers happen immediately, in insertion order, before timed events.
    if (!_pending_triggers.empty()) {
        TriggerTarget *target = _pending_triggers.front();
        _pending_triggers.pop_front();
        recordDispatch();
        target->activate();
        return true;
    }
    
    if (_pendingsources.empty())
        return false;
    
    simtime_picosec nexteventtime = _pendingsources.begin()->first.first;
    EventSource* nextsource = _pendingsources.begin()->second;
    _pendingsources.erase(_pendingsources.begin());
    assert(nexteventtime >= _lasteventtime);
    _lasteventtime = nexteventtime; // set this before calling doNextEvent, so that this::now() is accurate
    recordDispatch();
    nextsource->doNextEvent();
    return true;
}

EventList::EventKey
EventList::nextKey(simtime_picosec when)
{
    if (_next_insertion_order == numeric_limits<uint64_t>::max()) {
        cerr << "EventList insertion-order counter exhausted. Abort." << endl;
        abort();
    }
    return make_pair(when, _next_insertion_order++);
}

void
EventList::recordDispatch()
{
    if (_processed_event_count == numeric_limits<uint64_t>::max()) {
        cerr << "EventList processed-event counter exhausted. Abort." << endl;
        abort();
    }
    _processed_event_count++;
}

bool
EventList::nextEventTime(simtime_picosec& when)
{
    if (!_pending_triggers.empty()) {
        when = now();
        return true;
    }
    if (_pendingsources.empty())
        return false;
    when = _pendingsources.begin()->first.first;
    return true;
}


void
EventList::sourceIsPending(EventSource &src, simtime_picosec when)
{
    assert(when>=now());
    if (_endtime==0 || when<_endtime)
        _pendingsources.insert(make_pair(nextKey(when),&src));
}

EventList::Handle
EventList::sourceIsPendingGetHandle(EventSource &src, simtime_picosec when) 
{
    assert(when>=now());
    if (_endtime==0 || when<_endtime) {
        EventList::Handle handle =_pendingsources.insert(make_pair(nextKey(when),&src)).first;
        return handle;
    }
    return _pendingsources.end();
}

void
EventList::triggerIsPending(TriggerTarget &target) {
    _pending_triggers.push_back(&target);
}

void 
EventList::cancelPendingSource(EventSource &src) {
    pendingsources_t::iterator i = _pendingsources.begin();
    while (i != _pendingsources.end()) {
        if (i->second == &src) {
            _pendingsources.erase(i);
            return;
        }
        i++;
    }
}

void 
EventList::cancelPendingSourceByTime(EventSource &src, simtime_picosec when) {
    // fast cancellation of a timer - the timer MUST exist
    // this should normally be fast, except if we have a lot of events with exactly the same time value

    EventKey first = make_pair(when, 0);
    EventKey last = make_pair(when, numeric_limits<uint64_t>::max());
    auto range = make_pair(_pendingsources.lower_bound(first),
                           _pendingsources.upper_bound(last));

    for (auto i = range.first; i != range.second; ++i) {
        if (i->second == &src) {
            _pendingsources.erase(i);
            return;
        }
    }
    abort();
}


void EventList::cancelPendingSourceByHandle(EventSource &src, EventList::Handle handle) {
    // If we're cancelling timers often, cancel them by handle.  But
    // be careful - cancelling a handle that has already been
    // cancelled or has already expired is undefined behaviour
    assert(handle != _pendingsources.end());
    assert(handle->second == &src);
    assert(handle->first.first >= now());
    
    _pendingsources.erase(handle);
}

void 
EventList::reschedulePendingSource(EventSource &src, simtime_picosec when) {
    cancelPendingSource(src);
    sourceIsPending(src, when);
}

EventSource::EventSource(const string& name) : EventSource(EventList::getTheEventList(), name) 
{
}
