/// \file  TextLog.cpp
/// \brief Caliper text log service

#include "../CaliperService.h"

#include "SnapshotTextFormatter.h"

#include <Caliper.h>
#include <EntryList.h>

#include <Log.h>
#include <RuntimeConfig.h>

#include <util/split.hpp>

#include <algorithm>
#include <fstream>
#include <functional>
#include <iterator>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <sstream>
#include <vector>

using namespace cali;
using namespace std;

namespace
{

const ConfigSet::Entry   configdata[] = {
    { "trigger", CALI_TYPE_STRING, "",
      "List of attributes for which to write text log entries",
      "Colon-separated list of attributes for which to write text log entries."
    },
    { "formatstring", CALI_TYPE_STRING, "",
      "Format of the text log output",
      "Description of the text log format output. If empty, a default one will be created."
    },
    { "filename", CALI_TYPE_STRING, "stdout",
      "File name for event record stream. Auto-generated by default.",
      "File name for event record stream. Either one of\n"
      "   stdout: Standard output stream,\n"
      "   stderr: Standard error stream,\n"
      "   none:   No output,\n"
      " or a file name. The default is stdout\n"
    },
    ConfigSet::Terminator
};

class TextLogService
{
    ConfigSet                   config;

    std::mutex                  trigger_attr_mutex;
    typedef std::map<cali_id_t, Attribute> TriggerAttributeMap;
    TriggerAttributeMap         trigger_attr_map;

    std::vector<std::string>    trigger_attr_names;

    SnapshotTextFormatter       formatter;

    enum class Stream { None, File, StdErr, StdOut };

    Stream                      m_stream;
    ofstream                    m_ofstream;

    Attribute                   set_event_attr;   
    Attribute                   end_event_attr;

    std::mutex                  stream_mutex;
    
    static unique_ptr<TextLogService> 
                                s_textlog;

    std::string 
    create_default_formatstring(const std::vector<std::string>& attr_names) {
        if (attr_names.size() < 1)
            return "%time.inclusive.duration%";

        int name_sizes = 0;

        for (const std::string& s : attr_names)
            name_sizes += s.size();

        int w = max<int>(0, (80-10-name_sizes-2*attr_names.size()) / attr_names.size());

        std::ostringstream os;

        for (const std::string& s : attr_names)
            os << s << "=%[" << w << "]" << s << "% ";

        os << "%[8r]time.inclusive.duration%";

        return os.str();
    }

    void init_stream() {
        string filename = config.get("filename").to_string();

        const map<string, Stream> strmap { 
            { "none",   Stream::None   },
            { "stdout", Stream::StdOut },
            { "stderr", Stream::StdErr } };

        auto it = strmap.find(filename);

        if (it == strmap.end()) {
            m_ofstream.open(filename);

            if (!m_ofstream)
                Log(0).stream() << "Could not open text log file " << filename << endl;
            else
                m_stream = Stream::File;
        } else
            m_stream = it->second;
    }

    std::ostream& get_stream() {
        switch (m_stream) {
        case Stream::StdOut:
            return std::cout;
        case Stream::StdErr:
            return std::cerr;
        default:
            return m_ofstream;
        }
    }

    void create_attribute_cb(Caliper* c, const Attribute& attr) {
        formatter.update_attribute(attr);

        if (attr.skip_events())
            return;

        std::vector<std::string>::iterator it = 
            find(trigger_attr_names.begin(), trigger_attr_names.end(), attr.name());

        if (it != trigger_attr_names.end()) {
            std::lock_guard<std::mutex> lock(trigger_attr_mutex);
            trigger_attr_map.insert(std::make_pair(attr.id(), attr));
        }
    }

    void process_snapshot_cb(Caliper* c, const EntryList* trigger_info, const EntryList* snapshot) {
        // operate only on cali.snapshot.event.end attributes for now
        if (!trigger_info)
            return;

        Entry event = trigger_info->get(end_event_attr);

        if (event.is_empty())
            event = trigger_info->get(set_event_attr);
        if (event.is_empty())
            return;

        Attribute trigger_attr { Attribute::invalid };

        {
            std::lock_guard<std::mutex> lock(trigger_attr_mutex);

            TriggerAttributeMap::const_iterator it = 
                trigger_attr_map.find(event.value().to_id());

            if (it != trigger_attr_map.end())
                trigger_attr = it->second;
        }

        if (trigger_attr == Attribute::invalid || snapshot->get(trigger_attr).is_empty())
            return;

        ostringstream os;
        
        formatter.print(os, snapshot) << std::endl;

        std::lock_guard<std::mutex>
            g(stream_mutex);

        get_stream() << os.str();
    }

    void post_init_cb(Caliper* c) {
        std::string formatstr = config.get("formatstring").to_string();

        if (formatstr.size() == 0)
            formatstr = create_default_formatstring(trigger_attr_names);

        formatter.parse(formatstr, c);

        set_event_attr      = c->get_attribute("cali.snapshot.event.set");
        end_event_attr      = c->get_attribute("cali.snapshot.event.end");

        if (end_event_attr      == Attribute::invalid ||
            set_event_attr      == Attribute::invalid)
            Log(1).stream() << "TextLog: Note: \"event\" trigger attributes not registered\n"
                "    disabling text log.\n" << std::endl;
    }

    // static callbacks

    static void s_create_attribute_cb(Caliper* c, const Attribute& attr) { 
        s_textlog->create_attribute_cb(c, attr);
    }

    static void s_process_snapshot_cb(Caliper* c, const EntryList* trigger_info, const EntryList* snapshot) {
        s_textlog->process_snapshot_cb(c, trigger_info, snapshot);
    }

    static void s_post_init_cb(Caliper* c) { 
        s_textlog->post_init_cb(c);
    }

    TextLogService(Caliper* c)
        : config(RuntimeConfig::init("textlog", configdata)),
          set_event_attr(Attribute::invalid),
          end_event_attr(Attribute::invalid)
        { 
            init_stream();

            util::split(config.get("trigger").to_string(), ':', 
                        std::back_inserter(trigger_attr_names));

            c->events().create_attr_evt.connect(&TextLogService::s_create_attribute_cb);
            c->events().post_init_evt.connect(&TextLogService::s_post_init_cb);
            c->events().process_snapshot.connect(&TextLogService::s_process_snapshot_cb);

            Log(1).stream() << "Registered text log service" << std::endl;
        }

public:

    static void textlog_register(Caliper* c) {
        s_textlog.reset(new TextLogService(c));
    }

}; // TextLogService

unique_ptr<TextLogService> TextLogService::s_textlog { nullptr };

} // namespace

namespace cali
{
    CaliperService textlog_service = { "textlog", ::TextLogService::textlog_register };
} // namespace cali
