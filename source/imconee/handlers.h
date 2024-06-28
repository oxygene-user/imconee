#pragma once

#define MAXIMUM_SLOTS 30 // only 30 due each slot - two sockets, but maximum sockets per thread are 64

class listener;

class handler
{
protected:

	struct bridged
	{
		netkit::pipe_ptr pipe1;
		netkit::pipe_ptr pipe2;
		size_t mask1 = 0;
		size_t mask2 = 0;
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
		bool process(u8* data, netkit::pipe_waiter::mask &masks);
	};

	class processing_thread
	{
		volatile spinlock::long3264 sync = 0;
		netkit::pipe_waiter waiter;
		std::array<bridged, MAXIMUM_SLOTS> slots;
		signed_t numslots = 0;
		std::unique_ptr<processing_thread> next;

		void moveslot(signed_t to, signed_t from)
		{
			ASSERT(to <= from);
			if (to < from)
			{
				slots[to] = std::move(slots[from]);
				slots[from].pipe1 = nullptr;
				slots[from].pipe2 = nullptr;
			}
		}

	public:
		void signal()
		{
			waiter.signal();
		}
		void forward(processing_thread* to);
		processing_thread* get_next()
		{
			return next.get();
		}
		processing_thread* get_next_and_release()
		{
			processing_thread* n = next.get();
			next.release();
			return n;
		}
		std::unique_ptr<processing_thread>* get_next_ptr()
		{
			return &next;
		}
		void close();
		signed_t try_add_bridge(netkit::pipe* pipe1, netkit::pipe* pipe2)
		{
			spinlock::auto_simple_lock s(sync);
			if (numslots < 0)
				return -1;
			if (numslots < MAXIMUM_SLOTS)
			{
				slots[numslots].pipe1 = pipe1;
				slots[numslots].pipe2 = pipe2;
				slots[numslots].mask1 = 0;
				slots[numslots].mask2 = 0;

				ASSERT(numslots < MAXIMUM_SLOTS);
				++numslots;
				s.unlock();
				waiter.signal();
				return 1;
			}
			return 0;
		}
		signed_t check()
		{
			spinlock::auto_simple_lock s(sync);
			return numslots < 0 ? -1 : 0;
		}
		bool tick(u8 *data); // returns true 2 continue
	};


	struct state
	{
		std::unique_ptr<processing_thread> pth;
		//std::vector<netkit::pipe_ptr> pipes;
		signed_t idpool = 1;
		bool need_stop = false;
	};

	spinlock::syncvar<state> state;
	listener* owner;
	std::vector<const proxy*> proxychain;

	void bridge(netkit::pipe_ptr &&pipe1, netkit::pipe_ptr &&pipe2); // either does job in current thread or forwards job to another thread with same endpoint
	void bridge(processing_thread *npt);

public:
	handler(loader& ldr, listener* owner, const asts& bb);
	virtual ~handler() { stop(); }

	void stop();
	netkit::pipe_ptr connect(const netkit::endpoint& addr, bool direct); // just connect to remote host using current handler's proxy settings

	virtual void on_pipe(netkit::pipe* pipe)  // called from listener thread, execute as fast as possible
	{
		// so, this func is owner of pipe now
		// delete it
		// (override this to handle pipe)
		delete pipe; 
	}

	static handler* build(loader& ldr, listener *owner, const asts& bb);
};


class handler_direct : public handler // just port mapper
{
	std::string to_addr; // in format like: tcp://domain_or_ip:port

	void worker(netkit::pipe* pipe); // sends all from pipe to out connection

public:
	handler_direct( loader &ldr, listener* owner, const asts& bb );
	virtual ~handler_direct() { stop(); }

	/*virtual*/ void on_pipe(netkit::pipe* pipe) override;
};

class handler_socks : public handler // socks4 and socks5
{
	enum rslt
	{
		EC_GRANTED,
		EC_FAILED,
		EC_REMOTE_HOST_UNRCH,
	};

	std::string userid; // for socks4
	std::string login, pass; // for socks5

	bool socks5_allow_anon = false;
	
	bool allow_4 = true;
	bool allow_5 = true;

	void handshake4(netkit::pipe* pipe);
	void handshake5(netkit::pipe* pipe);

	void handshake(netkit::pipe* pipe);

	using sendanswer = std::function< void(netkit::pipe* pipe, rslt ecode) >;

	void worker(netkit::pipe* pipe, const netkit::endpoint& inf, sendanswer answ);

public:
	handler_socks(loader& ldr, listener* owner, const asts& bb, const std::string_view &st);
	virtual ~handler_socks() { stop(); }

	/*virtual*/ void on_pipe(netkit::pipe* pipe) override;
};

#include "handler_ss.h"
