// Copyright (c) 2016, Lawrence Livermore National Security, LLC.  
// Produced at the Lawrence Livermore National Laboratory.
//
// This file is part of Caliper.
// Written by David Boehme, boehme3@llnl.gov.
// LLNL-CODE-678900
// All rights reserved.
//
// For details, see https://github.com/scalability-llnl/Caliper.
// Please also see the LICENSE file for our additional BSD notice.
//
// Redistribution and use in source and binary forms, with or without modification, are
// permitted provided that the following conditions are met:
//
//  * Redistributions of source code must retain the above copyright notice, this list of
//    conditions and the disclaimer below.
//  * Redistributions in binary form must reproduce the above copyright notice, this list of
//    conditions and the disclaimer (as noted below) in the documentation and/or other materials
//    provided with the distribution.
//  * Neither the name of the LLNS/LLNL nor the names of its contributors may be used to endorse
//    or promote products derived from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS
// OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
// LAWRENCE LIVERMORE NATIONAL SECURITY, LLC, THE U.S. DEPARTMENT OF ENERGY OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
// WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
// ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "TraceBufferChunk.h"

#include <Caliper.h>
#include <EntryList.h>

#include <ContextRecord.h>
#include <Log.h>
#include <Node.h>
#include <RuntimeConfig.h>

#include <c-util/vlenc.h>

#define SNAP_MAX 80

using namespace trace;
using namespace cali;


TraceBufferChunk::~TraceBufferChunk()
{
    delete[] m_data;

    if (m_next)
        delete m_next;
}


void TraceBufferChunk::append(TraceBufferChunk* chunk)
{
    if (m_next)
        m_next->append(chunk);
    else
        m_next = chunk;
}


void TraceBufferChunk::reset()
{
    m_pos  = 0;
    m_nrec = 0;
            
    memset(m_data, 0, m_size);
}


size_t TraceBufferChunk::flush(Caliper* c, std::unordered_set<cali_id_t>& written_node_cache)
{
    size_t written = 0;

    //
    // local flush
    //

    size_t p = 0;

    for (size_t r = 0; r < m_nrec; ++r) {
        // decode snapshot record
                
        int n_nodes = static_cast<int>(std::min(static_cast<int>(vldec_u64(m_data + p, &p)), SNAP_MAX));
        int n_attr  = static_cast<int>(std::min(static_cast<int>(vldec_u64(m_data + p, &p)), SNAP_MAX));

        Variant node_vec[SNAP_MAX];
        Variant attr_vec[SNAP_MAX];
        Variant vals_vec[SNAP_MAX];

        for (int i = 0; i < n_nodes; ++i)
            node_vec[i] = Variant(static_cast<cali_id_t>(vldec_u64(m_data + p, &p)));
        for (int i = 0; i < n_attr;  ++i)
            attr_vec[i] = Variant(static_cast<cali_id_t>(vldec_u64(m_data + p, &p)));
        for (int i = 0; i < n_attr;  ++i)
            vals_vec[i] = Variant::unpack(m_data + p, &p, nullptr);

        // write nodes
        // FIXME: this node cache is a terrible kludge, needs to go away
        //   either make node-by-id lookup fast,
        //   or fix node-before-snapshot I/O requirement

        for (int i = 0; i < n_nodes; ++i) {
            cali_id_t node_id = node_vec[i].to_id();

            if (written_node_cache.count(node_id))
                continue;
                    
            Node* node = c->node(node_vec[i].to_id());
                    
            if (node)
                node->write_path(c->events().write_record);

            written_node_cache.insert(node_id);
        }
        for (int i = 0; i < n_attr; ++i) {
            cali_id_t node_id = attr_vec[i].to_id();

            if (written_node_cache.count(node_id))
                continue;

            Node* node = c->node(attr_vec[i].to_id());
                    
            if (node)
                node->write_path(c->events().write_record);

            written_node_cache.insert(node_id);
        }

        // write snapshot
                
        int               n[3] = {  n_nodes,   n_attr,   n_attr };
        const Variant* data[3] = { node_vec, attr_vec, vals_vec };

        c->events().write_record(ContextRecord::record_descriptor(), n, data);
    }

    written += m_nrec;            
    reset();
            
    //
    // flush subsequent buffers in list
    // 
            
    if (m_next) {
        written += m_next->flush(c, written_node_cache);
        delete m_next;
        m_next = 0;
    }
            
    return written;
}


void TraceBufferChunk::save_snapshot(const EntryList* s)
{
    EntryList::Sizes sizes = s->size();

    if ((sizes.n_nodes + sizes.n_immediate) == 0)
        return;

    sizes.n_nodes     = std::min<size_t>(sizes.n_nodes,     SNAP_MAX);
    sizes.n_immediate = std::min<size_t>(sizes.n_immediate, SNAP_MAX);
                
    m_pos += vlenc_u64(sizes.n_nodes,     m_data + m_pos);
    m_pos += vlenc_u64(sizes.n_immediate, m_data + m_pos);

    EntryList::Data addr = s->data();

    for (int i = 0; i < sizes.n_nodes; ++i)
        m_pos += vlenc_u64(addr.node_entries[i]->id(), m_data + m_pos);
    for (int i = 0; i < sizes.n_immediate;  ++i)
        m_pos += vlenc_u64(addr.immediate_attr[i],     m_data + m_pos);
    for (int i = 0; i < sizes.n_immediate;  ++i)
        m_pos += addr.immediate_data[i].pack(m_data + m_pos);

    ++m_nrec;
}


bool TraceBufferChunk::fits(const EntryList* s) const
{
    EntryList::Sizes sizes = s->size();

    // get worst-case estimate of packed snapshot size:
    //   20 bytes for size indicators
    //   10 bytes per node id
    //   10+22 bytes per immediate entry (10 for attr, 22 for variant)
            
    size_t max = 20 + 10 * sizes.n_nodes + 32 * sizes.n_immediate;

    return (m_pos + max) < m_size;
}


TraceBufferChunk::UsageInfo TraceBufferChunk::info() const
{
    UsageInfo info { 0, 0, 0 };

    if (m_next)
        info = m_next->info();

    info.nchunks++;
    info.reserved += m_size;
    info.used     += m_pos;

    return info;
}
