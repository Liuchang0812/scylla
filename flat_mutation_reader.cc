/*
 * Copyright (C) 2017 ScyllaDB
 */

/*
 * This file is part of Scylla.
 *
 * Scylla is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Scylla is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Scylla.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "flat_mutation_reader.hh"
#include "mutation_reader.hh"
#include <algorithm>

#include <boost/range/adaptor/transformed.hpp>
#include <seastar/util/defer.hh>

void flat_mutation_reader::impl::forward_buffer_to(schema_ptr& schema, const position_in_partition& pos) {
    _buffer.erase(std::remove_if(_buffer.begin(), _buffer.end(), [this, &pos, &schema] (mutation_fragment& f) {
        return !f.relevant_for_range_assuming_after(*schema, pos);
    }), _buffer.end());

    _buffer_size = boost::accumulate(_buffer | boost::adaptors::transformed(std::mem_fn(&mutation_fragment::memory_usage)), size_t(0));
}

void flat_mutation_reader::impl::clear_buffer_to_next_partition() {
    auto next_partition_start = std::find_if(_buffer.begin(), _buffer.end(), [] (const mutation_fragment& mf) {
        return mf.is_partition_start();
    });
    _buffer.erase(_buffer.begin(), next_partition_start);

    _buffer_size = boost::accumulate(_buffer | boost::adaptors::transformed(std::mem_fn(&mutation_fragment::memory_usage)), size_t(0));
}

flat_mutation_reader flat_mutation_reader_from_mutation_reader(schema_ptr s, mutation_reader&& legacy_reader, streamed_mutation::forwarding fwd) {
    class converting_reader final : public flat_mutation_reader::impl {
        schema_ptr _schema;
        mutation_reader _legacy_reader;
        streamed_mutation_opt _sm;
        streamed_mutation::forwarding _fwd;

        future<> get_next_sm() {
            return _legacy_reader().then([this] (auto&& sm) {
                if (bool(sm)) {
                    _sm = std::move(sm);
                    this->push_mutation_fragment(
                            mutation_fragment(partition_start(_sm->decorated_key(), _sm->partition_tombstone())));
                } else {
                    _end_of_stream = true;
                }
            });
        }
        void on_sm_finished() {
            if (_fwd == streamed_mutation::forwarding::yes) {
                _end_of_stream = true;
            } else {
                this->push_mutation_fragment(mutation_fragment(partition_end()));
                _sm = {};
            }
        }
    public:
        converting_reader(schema_ptr s, mutation_reader&& legacy_reader, streamed_mutation::forwarding fwd)
            : _schema(std::move(s)), _legacy_reader(std::move(legacy_reader)), _fwd(fwd)
        { }
        virtual future<> fill_buffer() override {
            return do_until([this] { return is_end_of_stream() || is_buffer_full(); }, [this] {
                if (!_sm) {
                    return get_next_sm();
                } else {
                    if (_sm->is_buffer_empty()) {
                        if (_sm->is_end_of_stream()) {
                            on_sm_finished();
                            return make_ready_future<>();
                        }
                        return _sm->fill_buffer();
                    } else {
                        while (!_sm->is_buffer_empty() && !is_buffer_full()) {
                            this->push_mutation_fragment(_sm->pop_mutation_fragment());
                        }
                        if (_sm->is_end_of_stream() && _sm->is_buffer_empty()) {
                            on_sm_finished();
                        }
                        return make_ready_future<>();
                    }
                }
            });
        }
        virtual void next_partition() override {
            if (_fwd == streamed_mutation::forwarding::yes) {
                clear_buffer();
                _sm = {};
                _end_of_stream = false;
            } else {
                clear_buffer_to_next_partition();
                if (_sm && is_buffer_empty()) {
                    _sm = {};
                }
            }
        }
        virtual future<> fast_forward_to(const dht::partition_range& pr) override {
            clear_buffer();
            _sm = { };
            _end_of_stream = false;
            return _legacy_reader.fast_forward_to(pr);
        };
        virtual future<> fast_forward_to(position_range cr) override {
            forward_buffer_to(_schema, cr.start());
            _end_of_stream = false;
            if (_sm) {
                return _sm->fast_forward_to(std::move(cr));
            } else {
                throw std::runtime_error("fast forward needs _sm to be set");
            }
        };
    };
    return make_flat_mutation_reader<converting_reader>(std::move(s), std::move(legacy_reader), fwd);
}

flat_mutation_reader make_forwardable(schema_ptr s, flat_mutation_reader m) {
    class reader : public flat_mutation_reader::impl {
        schema_ptr _schema;
        flat_mutation_reader _underlying;
        position_range _current = {
            position_in_partition(position_in_partition::partition_start_tag_t()),
            position_in_partition(position_in_partition::after_static_row_tag_t())
        };
        mutation_fragment_opt _next;
        // When resolves, _next is engaged or _end_of_stream is set.
        future<> ensure_next() {
            if (_next) {
                return make_ready_future<>();
            }
            return _underlying().then([this] (auto&& mfo) {
                _next = std::move(mfo);
                if (!_next) {
                    _end_of_stream = true;
                }
            });
        }
    public:
        reader(schema_ptr s, flat_mutation_reader r) : _schema(std::move(s)), _underlying(std::move(r)) { }
        virtual future<> fill_buffer() override {
            return repeat([this] {
                if (is_buffer_full()) {
                    return make_ready_future<stop_iteration>(stop_iteration::yes);
                }
                return ensure_next().then([this] {
                    if (is_end_of_stream()) {
                        return stop_iteration::yes;
                    }
                    position_in_partition::less_compare cmp(*_schema);
                    if (!cmp(_next->position(), _current.end())) {
                        _end_of_stream = true;
                        // keep _next, it may be relevant for next range
                        return stop_iteration::yes;
                    }
                    if (_next->relevant_for_range(*_schema, _current.start())) {
                        push_mutation_fragment(std::move(*_next));
                    }
                    _next = {};
                    return stop_iteration::no;
                });
            });
        }
        virtual future<> fast_forward_to(position_range pr) override {
            _current = std::move(pr);
            _end_of_stream = false;
            forward_buffer_to(_schema, _current.start());
            return make_ready_future<>();
        }
        virtual void next_partition() override {
            _end_of_stream = false;
            if (!_next || !_next->is_partition_start()) {
                _underlying.next_partition();
                _next = {};
            }
            clear_buffer_to_next_partition();
            _current = {
                position_in_partition(position_in_partition::partition_start_tag_t()),
                position_in_partition(position_in_partition::after_static_row_tag_t())
            };
        }
        virtual future<> fast_forward_to(const dht::partition_range& pr) override {
            clear_buffer();
            _next = {};
            return _underlying.fast_forward_to(pr);
        }
    };
    return make_flat_mutation_reader<reader>(std::move(s), std::move(m));
}

class empty_flat_reader final : public flat_mutation_reader::impl {
public:
    empty_flat_reader() { _end_of_stream = true; }
    virtual future<> fill_buffer() override { return make_ready_future<>(); }
    virtual void next_partition() override {}
    virtual future<> fast_forward_to(const dht::partition_range& pr) override { return make_ready_future<>(); };
    virtual future<> fast_forward_to(position_range cr) override { return make_ready_future<>(); };
};

flat_mutation_reader make_empty_flat_reader() {
    return make_flat_mutation_reader<empty_flat_reader>();
}

streamed_mutation streamed_mutation_from_mutation_copy(mutation m, streamed_mutation::forwarding fwd)
{
    class reader final : public streamed_mutation::impl {
        mutation _mutation;
        position_in_partition::less_compare _cmp;
        bool _static_row_done = false;
        mutation_fragment_opt _rt;
        mutation_fragment_opt _cr;
    private:
        void prepare_next_clustering_row() {
            auto& crs = _mutation.partition().clustered_rows();
            while (true) {
                auto re = crs.unlink_leftmost_without_rebalance();
                if (!re) {
                    break;
                }
                auto re_deleter = defer([re] { current_deleter<rows_entry>()(re); });
                if (!re->dummy()) {
                    _cr = mutation_fragment(std::move(*re));
                    break;
                }
            }
        }
        void prepare_next_range_tombstone() {
            auto& rts = _mutation.partition().row_tombstones().tombstones();
            auto rt = rts.unlink_leftmost_without_rebalance();
            if (rt) {
                auto rt_deleter = defer([rt] { current_deleter<range_tombstone>()(rt); });
                _rt = mutation_fragment(std::move(*rt));
            }
        }
        mutation_fragment_opt read_next() {
            if (_cr && (!_rt || _cmp(_cr->position(), _rt->position()))) {
                auto cr = move_and_disengage(_cr);
                prepare_next_clustering_row();
                return cr;
            } else if (_rt) {
                auto rt = move_and_disengage(_rt);
                prepare_next_range_tombstone();
                return rt;
            }
            return { };
        }
    private:
        void do_fill_buffer() {
            if (!_static_row_done) {
                _static_row_done = true;
                if (!_mutation.partition().static_row().empty()) {
                    push_mutation_fragment(static_row(std::move(_mutation.partition().static_row())));
                }
            }
            while (!is_end_of_stream() && !is_buffer_full()) {
                auto mfopt = read_next();
                if (mfopt) {
                    push_mutation_fragment(std::move(*mfopt));
                } else {
                    _end_of_stream = true;
                }
            }
        }
    public:
        explicit reader(mutation m)
            : streamed_mutation::impl(m.schema(), m.decorated_key(), m.partition().partition_tombstone())
            , _mutation(std::move(m))
            , _cmp(*_mutation.schema())
        {
            auto mutation_destroyer = defer([this] { destroy_mutation(); });

            prepare_next_clustering_row();
            prepare_next_range_tombstone();

            do_fill_buffer();

            mutation_destroyer.cancel();
        }

        void destroy_mutation() noexcept {
            // After unlink_leftmost_without_rebalance() was called on a bi::set
            // we need to complete destroying the tree using that function.
            // clear_and_dispose() used by mutation_partition destructor won't
            // work properly.

            auto& crs = _mutation.partition().clustered_rows();
            auto re = crs.unlink_leftmost_without_rebalance();
            while (re) {
                current_deleter<rows_entry>()(re);
                re = crs.unlink_leftmost_without_rebalance();
            }

            auto& rts = _mutation.partition().row_tombstones().tombstones();
            auto rt = rts.unlink_leftmost_without_rebalance();
            while (rt) {
                current_deleter<range_tombstone>()(rt);
                rt = rts.unlink_leftmost_without_rebalance();
            }
        }

        ~reader() {
            destroy_mutation();
        }

        virtual future<> fill_buffer() override {
            do_fill_buffer();
            return make_ready_future<>();
        }
    };

    auto sm = make_streamed_mutation<reader>(std::move(m));
    if (fwd) {
        return make_forwardable(std::move(sm)); // FIXME: optimize
    }
    return std::move(sm);
}
