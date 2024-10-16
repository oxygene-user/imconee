#pragma once

#define MAXIMUM_SLOTS 30 // only 30 due each slot - two sockets, but maximum sockets per thread are 64

class listener;

#ifdef LOG_TRAFFIC
class traffic_logger
{
	signed_t id;
	HANDLE f21 = nullptr;
	HANDLE f12 = nullptr;
	str::astr fn;

	traffic_logger(traffic_logger&) = delete;
	traffic_logger& operator=(traffic_logger&) = delete;
	void prepare();
public:
	traffic_logger();
	~traffic_logger();
	traffic_logger& operator=(traffic_logger&&);
	void clear();
	void log12(u8* data, signed_t sz);
	void log21(u8* data, signed_t sz);
};
#endif

class handler
{
protected:

	struct bridged
	{
		netkit::pipe_ptr pipe1;
		netkit::pipe_ptr pipe2;
		size_t mask1 = 0;
		size_t mask2 = 0;

#ifdef LOG_TRAFFIC
		traffic_logger loger;
#endif

		void clear()
		{
			pipe1 = nullptr;
			pipe2 = nullptr;
			mask1 = 0;
			mask2 = 0;
#ifdef LOG_TRAFFIC
			loger.clear();
#endif
		}

		bool is_empty() const
		{
			return pipe1 == nullptr || pipe2 == nullptr;
		}

		bool prepare_wait(netkit::pipe_waiter& w)
		{
			mask1 = w.reg(pipe1); if (mask1 == 0) return false;
			mask2 = w.reg(pipe2); if (mask2 == 0)
			{
				w.unreg_last();
				return false;
			}
			return true;
		}

		enum process_result
		{
			SLOT_DEAD,
			SLOT_PROCESSES,
			SLOT_SKIPPED,
		};

		process_result process(u8* data, netkit::pipe_waiter::mask& masks); // returns: -1 - dead slot, 0 - slot not processed, 1 - slot processed

	};

	class tcp_processing_thread
	{
		//volatile spinlock::long3264 sync = 0;
		spinlock::syncvar<signed_t> numslots;
		netkit::pipe_waiter waiter;
		std::array<bridged, MAXIMUM_SLOTS> slots;
		std::unique_ptr<tcp_processing_thread> next;

		void moveslot(signed_t to, signed_t from)
		{
			ASSERT(to <= from);
			if (to < from)
			{
				slots[to] = std::move(slots[from]);
			}
			slots[from].pipe1 = nullptr;
			slots[from].pipe2 = nullptr;
#ifdef LOG_TRAFFIC
			slots[from].loger.clear();
#endif
		}

	public:
		void signal()
		{
			waiter.signal();
		}
		bool transplant(signed_t slot, tcp_processing_thread* to);
		tcp_processing_thread* get_next()
		{
			return next.get();
		}
		tcp_processing_thread* get_next_and_forget()
		{
			tcp_processing_thread* n = next.get();
			next.release();
			return n;
		}
		std::unique_ptr<tcp_processing_thread>* get_next_ptr()
		{
			return &next;
		}
		void close();
		signed_t try_add_bridge(netkit::pipe* pipe1, netkit::pipe* pipe2)
		{
			auto ns = numslots.lock_read();
			if (ns() < 0)
				return -1;
			if (ns() < MAXIMUM_SLOTS)
			{
				ns.unlock();
				auto nsw = numslots.lock_write();

				// check again
				if (nsw() < 0)
					return -1;
				if (nsw() >= MAXIMUM_SLOTS)
					return 0;

				ASSERT(slots[nsw()].pipe1 == nullptr);
				ASSERT(slots[nsw()].pipe2 == nullptr);

				slots[nsw()].pipe1 = pipe1;
				slots[nsw()].pipe2 = pipe2;
				slots[nsw()].mask1 = 0;
				slots[nsw()].mask2 = 0;
				++nsw();
				return 1;
			}
			return 0;
		}
		/*
		signed_t check()
		{
			return numslots.lock_read()() < 0 ? -1 : 0;
		}
		*/
		signed_t tick(u8 *data); // returns -1 to stop, 0..numslots - inactive slot index (4 forward), >MAXIMUM_SLOTS - do nothing
	};

	struct send_data
	{
        netkit::endpoint tgt;
        signed_t datasz;

		static consteval signed_t plain_tail_start() // get size of plain tail of this structure
		{
			return netkit::endpoint::plain_tail_start(); /* + offsetof(send_data, tgt) */
		}

		u8* data()
		{
			return ((u8 *)(this+1));
		}
        const u8* data() const
        {
			return ((const u8*)(this+1));
        }

		static u16 pre()
		{
			return tools::as_word(sizeof(send_data) - plain_tail_start());
		}

		static send_data* build(std::span<const u8> data, const netkit::endpoint& tgt)
		{
			if (send_data* sd = (send_data*)malloc(sizeof(send_data) + data.size()))
            {
				new (sd) send_data(tgt, data.size());
                memcpy(sd->data(), data.data(), data.size());
				return sd;
			}
			return nullptr;
		}

	private:
        send_data(const netkit::endpoint &tgt, signed_t datasz):tgt(tgt), datasz(datasz) {} // do not allow direct creation

	};

	struct mfrees
	{
		void operator()(send_data* p)
		{
			p->~send_data();
			free(p);
		}
	};

	using sdptr = std::unique_ptr<send_data, mfrees>;

	class udp_processing_thread : public netkit::udp_pipe, public ptr::sync_shared_object
	{
		netkit::thread_storage ts; // for udp send
		netkit::ipap hashkey;
		signed_t cutoff_time = 0;
		signed_t timeout = 5000;
		const proxy* prx = nullptr;
		spinlock::syncvar<tools::fifo<sdptr>> sendb;
		netkit::udp_pipe *sendor = nullptr;

	public:

		udp_processing_thread(const proxy* prx, signed_t timeout, const netkit::ipap & k /*, bool v4*/):prx(prx), timeout(timeout), hashkey(k)
		{
			//udp_prepare(ts, v4);
		}
		const netkit::ipap& key() const
		{
			return hashkey;
		}

		void update_cutoff_time()
		{
			cutoff_time = chrono::ms() + timeout;
		}
		void close();

		void convey(netkit::udp_packet &p, const netkit::endpoint& tgt);

		/*virtual*/ netkit::io_result send(const netkit::endpoint& toaddr, const netkit::pgen& pg) override;
		/*virtual*/ netkit::io_result recv(netkit::pgen& pg, signed_t max_bufer_size) override;

		bool is_timeout( signed_t curtime ) const
		{
			return cutoff_time != 0 && curtime >= cutoff_time;
		}

		void udp_bridge(SOCKET initiator);

	};

	spinlock::syncvar<std::unique_ptr<tcp_processing_thread>> tcp_pth; // list of tcp threads
	std::unordered_map<netkit::ipap, ptr::shared_ptr<udp_processing_thread>> udp_pth; // only accept thread can modify this map
	spinlock::syncvar<std::vector<netkit::ipap>> finished; // keys of finished threads

	listener* owner;
	std::vector<const proxy*> proxychain;

	volatile bool need_stop = false;

	void bridge(netkit::pipe_ptr &&pipe1, netkit::pipe_ptr &&pipe2); // either does job in current thread or forwards job to another thread with same endpoint
	void bridge(tcp_processing_thread *npt);

	void release_udps(); // must be called from listener thread
	void release_udp(udp_processing_thread *udp_wt);
	void release_tcp(tcp_processing_thread* udp_wt);

public:
	handler(loader& ldr, listener* owner, const asts& bb);
	virtual ~handler() { stop(); }

	void stop();
	netkit::pipe_ptr connect(netkit::endpoint& addr, bool direct); // just connect to remote host using current handler's proxy settings

	virtual str::astr desc() const = 0;
	virtual bool compatible(netkit::socket_type /*st*/) const
	{
		return false;
	}

	virtual void on_pipe(netkit::pipe* pipe)  // called from listener thread, execute as fast as possible
	{
		// so, this func is owner of pipe now
		// delete it
		// (override this to handle pipe)
		delete pipe; 
	}

	virtual void on_udp(netkit::socket &, netkit::udp_packet& )
	{
		// do nothing by default
	}


	static handler* build(loader& ldr, listener *owner, const asts& bb, netkit::socket_type st);
};


class handler_direct : public handler // just port mapper
{
	str::astr to_addr; // in format like: tcp://domain_or_ip:port
	netkit::endpoint ep; // only accessed from listener thread

	void tcp_worker(netkit::pipe* pipe); // sends all from pipe to out connection
	void udp_worker(netkit::socket* lstnr, udp_processing_thread* udp_wt);
	signed_t udp_timeout_ms = 5000;

public:
	handler_direct( loader &ldr, listener* owner, const asts& bb, netkit::socket_type st );
	virtual ~handler_direct() { stop(); }

	/*virtual*/ str::astr desc() const { return str::astr(ASTR("direct")); }
	/*virtual*/ bool compatible(netkit::socket_type /*st*/) const
	{
		return true; // compatible with both tcp and udp
	}

	/*virtual*/ void on_pipe(netkit::pipe* pipe) override;
	/*virtual*/ void on_udp(netkit::socket& lstnr, netkit::udp_packet& p) override;
};

class handler_socks : public handler // socks4 and socks5
{
	enum rslt
	{
		EC_GRANTED,
		EC_FAILED,
		EC_REMOTE_HOST_UNRCH,
	};

	str::astr userid; // for socks4
	str::astr login, pass; // for socks5

	bool socks5_allow_anon = false;
	
	bool allow_4 = true;
	bool allow_5 = true;

	void handshake4(netkit::pipe* pipe);
	void handshake5(netkit::pipe* pipe);

	void handshake(netkit::pipe* pipe);

	using sendanswer = std::function< void(netkit::pipe* pipe, rslt ecode) >;

	void worker(netkit::pipe* pipe, netkit::endpoint& inf, sendanswer answ);

public:
	handler_socks(loader& ldr, listener* owner, const asts& bb, const str::astr_view &st);
	virtual ~handler_socks() { stop(); }

	/*virtual*/ str::astr desc() const { return str::astr(ASTR("socks")); }
	/*virtual*/ bool compatible(netkit::socket_type st) const
	{
		return st == netkit::ST_TCP;
	}

	/*virtual*/ void on_pipe(netkit::pipe* pipe) override;
};

#include "handler_ss.h"
