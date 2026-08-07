#include <functional>
#include <iostream>
#include <string>
#include <string_view>

#include "eventlist.h"
#include "trigger.h"

namespace {

class RecordingSource final : public EventSource {
  public:
    RecordingSource(EventList& event_list, char marker, std::string& trace,
                    std::function<void()> callback = {})
        : EventSource(event_list, "recording_source"),
          marker_(marker),
          trace_(trace),
          callback_(std::move(callback)) {}

    void doNextEvent() override {
        trace_.push_back(marker_);
        if (callback_) {
            callback_();
        }
    }

  private:
    char marker_;
    std::string& trace_;
    std::function<void()> callback_;
};

class RecordingTrigger final : public TriggerTarget {
  public:
    RecordingTrigger(char marker, std::string& trace)
        : marker_(marker), trace_(trace) {}

    void activate() override { trace_.push_back(marker_); }

  private:
    char marker_;
    std::string& trace_;
};

bool drain() {
    while (EventList::doNextEvent()) {
    }
    return true;
}

bool timed_fifo() {
    EventList event_list;
    std::string trace;
    RecordingSource first(event_list, 'A', trace);
    RecordingSource second(event_list, 'B', trace);
    EventList::sourceIsPending(first, 10);
    EventList::sourceIsPending(second, 10);
    drain();
    return trace == "AB" && EventList::now() == 10 &&
           EventList::processedEventCount() == 2;
}

bool callback_insertion() {
    EventList event_list;
    std::string trace;
    RecordingSource inserted(event_list, 'C', trace);
    RecordingSource first(event_list, 'A', trace, [&inserted]() {
        EventList::sourceIsPending(inserted, EventList::now());
    });
    RecordingSource second(event_list, 'B', trace);
    EventList::sourceIsPending(first, 10);
    EventList::sourceIsPending(second, 10);
    drain();
    return trace == "ABC" && EventList::processedEventCount() == 3;
}

bool trigger_fifo_before_timed() {
    EventList event_list;
    std::string trace;
    RecordingSource timed(event_list, 'T', trace);
    RecordingTrigger first('A', trace);
    RecordingTrigger second('B', trace);
    EventList::sourceIsPending(timed, 1);
    EventList::triggerIsPending(first);
    EventList::triggerIsPending(second);

    simtime_picosec next_time = 99;
    const bool query_before = EventList::nextEventTime(next_time) && next_time == 0 &&
                              EventList::pendingTriggerCount() == 2 &&
                              EventList::pendingTimedEventCount() == 1;
    drain();
    return query_before && trace == "ABT" &&
           EventList::processedEventCount() == 3;
}

bool cancel_and_reschedule() {
    EventList event_list;
    std::string trace;
    RecordingSource by_source(event_list, 'A', trace);
    RecordingSource by_time(event_list, 'B', trace);
    RecordingSource by_handle(event_list, 'C', trace);
    RecordingSource rescheduled(event_list, 'D', trace);

    EventList::sourceIsPending(by_source, 5);
    EventList::sourceIsPending(by_time, 6);
    const EventList::Handle handle =
        EventList::sourceIsPendingGetHandle(by_handle, 7);
    EventList::sourceIsPending(rescheduled, 8);
    EventList::cancelPendingSource(by_source);
    EventList::cancelPendingSourceByTime(by_time, 6);
    EventList::cancelPendingSourceByHandle(by_handle, handle);
    EventList::reschedulePendingSource(rescheduled, 4);

    simtime_picosec next_time = 0;
    const bool query_before = EventList::nextEventTime(next_time) && next_time == 4 &&
                              EventList::pendingTimedEventCount() == 1;
    drain();
    simtime_picosec unused = 0;
    return query_before && trace == "D" && EventList::now() == 4 &&
           EventList::processedEventCount() == 1 &&
           EventList::pendingTimedEventCount() == 0 &&
           !EventList::nextEventTime(unused);
}

bool endtime_boundary() {
    EventList event_list;
    EventList::setEndtime(10);
    std::string trace;
    RecordingSource accepted(event_list, 'A', trace);
    RecordingSource rejected(event_list, 'B', trace);
    EventList::sourceIsPending(accepted, 9);
    EventList::sourceIsPending(rejected, 10);
    drain();
    return trace == "A" && EventList::now() == 9 &&
           EventList::processedEventCount() == 1;
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "usage: test_eventlist_order <case>\n";
        return 2;
    }

    const std::string_view test_case(argv[1]);
    bool passed = false;
    if (test_case == "timed-fifo") {
        passed = timed_fifo();
    } else if (test_case == "callback-insertion") {
        passed = callback_insertion();
    } else if (test_case == "trigger-fifo") {
        passed = trigger_fifo_before_timed();
    } else if (test_case == "cancel-reschedule") {
        passed = cancel_and_reschedule();
    } else if (test_case == "endtime-boundary") {
        passed = endtime_boundary();
    } else {
        std::cerr << "unknown test case: " << test_case << '\n';
        return 2;
    }

    if (!passed) {
        std::cerr << "eventlist test failed: " << test_case << '\n';
        return 1;
    }
    std::cout << test_case << ": PASS\n";
    return 0;
}
