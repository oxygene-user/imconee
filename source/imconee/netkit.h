#pragma once

#ifdef _WIN32
#include <winsock2.h>
#include <Mmsystem.h>
#endif

#define LOOP_PERIOD 500000 // 0.5 sec

namespace netkit
{
	template <bool native_little> struct cvt {};
	template<> struct cvt<true> { 
		static inline u16 to_ne(u16 v_nitive) // convert to network-endian
		{
			return ((v_nitive & 0xff) << 8) | ((v_nitive >> 8) & 0xff);
		}
		static inline u16 to_he(u16 v_network) // convert to host-endian
		{
			return ((v_network & 0xff) << 8) | ((v_network >> 8) & 0xff);
		}
	};
	template<> struct cvt<false> {
		static inline u16 to_ne(u16 v_nitive) // convert to network-endian
		{
			return v_nitive;
		}
		static inline u16 to_he(u16 v_network) // convert to host-endian
		{
			return v_network;
		}
	};

	inline u16 to_ne(u16 v_nitive)
	{
		return cvt<Endian::little>::to_ne(v_nitive);
	}
	inline u16 to_he(u16 v_network)
	{
		return cvt<Endian::little>::to_he(v_network);
	}


	struct ip4 : public in_addr
	{
		static ip4 parse(const std::string_view& s);

		ip4() { S_un.S_addr = 0; }
		ip4(const sockaddr_in &a) {
			this->S_un.S_addr = 0;
			if (a.sin_family == AF_INET)
			{
				this->S_un = a.sin_addr.S_un;
			}
		}

		void operator=(const sockaddr_in* ipv4)
		{
			S_un.S_addr = 0;
			this->S_un = ipv4->sin_addr.S_un;
		}

		void operator=(const addrinfo *addr)
		{
			S_un.S_addr = 0;

			for (; addr != nullptr; addr = addr->ai_next) {
				//void* addr;
				//char* ipver;

				// get the pointer to the address itself,
				// different fields in IPv4 and IPv6:
				if (addr->ai_family == AF_INET) { // IPv4
					struct sockaddr_in* ipv4 = (struct sockaddr_in*)addr->ai_addr;
					//addr = &(ipv4->sin_addr);
					//ipver = "IPv4";

					this->S_un = ipv4->sin_addr.S_un;
				}
				else { // IPv6
					//struct sockaddr_in6* ipv6 = (struct sockaddr_in6*)p->ai_addr;
					//addr = &(ipv6->sin6_addr);
					//ipver = "IPv6";
				}

				// convert the IP to a string and print it:
				//inet_ntop(p->ai_family, addr, ipstr, sizeof ipstr);
				//printf("  %s: %s\n", ipver, ipstr);
			}
		}

		std::string to_string() const
		{
			std::string s = std::to_string( S_un.S_un_b.s_b1);
			s.push_back('.'); s.append(std::to_string(S_un.S_un_b.s_b2));
			s.push_back('.'); s.append(std::to_string(S_un.S_un_b.s_b3));
			s.push_back('.'); s.append(std::to_string(S_un.S_un_b.s_b4));
			return s;
		}

		operator u32() const {
			return S_un.S_addr;
		}
	};

	enum addr_type
	{
		AT_ERROR,
		AT_TCP_DOMAIN,
		AT_TCP_RESLOVED,
	};

	enum wrslt
	{
		WR_TIMEOUT,
		WR_READY4READ,
		WR_CLOSED,
	};

#ifdef _WIN32
	struct waitable_data
	{
		WSAEVENT wsaevent = nullptr;
		SOCKET s = INVALID_SOCKET;
		signed_t ready = 0; // |1 if WSAEnumNetworkEvents already returns FD_READ for socket
		void operator=(SOCKET s_) { s = s_; }
	};
	using WAITABLE = waitable_data*;
	inline bool is_ready(WAITABLE w) { return w->ready != 0; }
	inline void make_ready(WAITABLE w, signed_t mask) { w->ready |= mask; }
	inline void clear_ready(WAITABLE w, signed_t mask) { w->ready &= ~mask; }
#define NULL_WAITABLE ((netkit::WAITABLE)nullptr)
#define MAXIMUM_WAITABLES (MAXIMUM_WAIT_OBJECTS - 2)
#endif

	wrslt wait(WAITABLE s, long microsec);

	struct pipe;
	class pipe_waiter
	{
		using ppipe = pipe*;
		ppipe pipes[MAXIMUM_WAITABLES];
#ifdef _WIN32
		SOCKET soks[MAXIMUM_WAITABLES + 2];
		WSAEVENT www[MAXIMUM_WAITABLES + 2];
		WSAEVENT sig = nullptr;
#endif
		signed_t numw = 0;
		u64 readymask = 0;

		static_assert(MAXIMUM_WAITABLES <= 64);

	public:

		class mask
		{
			u64 readm = 0;
			u64 closem = 0;
			u64 writem = 0;

		public:
			mask(u64 readmask = 0):readm(readmask)
			{
			}

			bool have_read(u64 m)
			{
				if ((readm & m) != 0)
				{
					readm &= ~m;
					return true;
				}
				return false;
			}
			bool have_closed(u64 m)
			{
				if ((closem & m) != 0)
				{
					closem &= ~m;
					return true;
				}
				return false;
			}
			bool have_write(u64 m)
			{
				if ((writem & m) != 0)
				{
					writem &= ~m;
					return true;
				}
				return false;
			}

			void add_read(u64 m) { readm |= m; }
			void add_close(u64 m) { closem |= m; }
			void add_write(u64 m) { writem |= m; }

			void remove_read(u64 m) { readm &= ~m; }
			void remove_close(u64 m) { closem &= ~m; }
			void remove_write(u64 m) { writem &= ~m; }

			bool is_empty() const { return readm == 0 && closem == 0 && writem == 0; }
		};

		~pipe_waiter()
		{
#ifdef _WIN32
			if (sig)
				WSACloseEvent(sig);
#endif
		}

		u64 reg(pipe* p);
		void unreg_last();

		mask wait(long microsec); // after wait return, waiter is in empty state
		void signal();
	};

	class endpoint
	{
		std::string domain_;
		mutable ip4 ip = {};
		signed_t port_ = 0;
		mutable addr_type type_ = AT_ERROR;

	public:

		endpoint() {}
		explicit endpoint(const std::string& addr);
		endpoint(ip4 ip, signed_t port):ip(ip), port_(port), type_(AT_TCP_RESLOVED) {}

		bool operator==(const endpoint& ep)
		{
			switch (type_)
			{
			case AT_TCP_DOMAIN:

				if (ep.type_ == AT_TCP_DOMAIN)
					return domain_ == ep.domain_;

				if (ep.type_ == AT_TCP_RESLOVED)
					return port_ == ep.port_ && ip == ep.ip;

				return false;
			case netkit::AT_TCP_RESLOVED:
				return port_ == ep.port_ && ip == ep.ip;
			}
			return false;
		}

		void preparse(const std::string& addr);

		void set_ip4(ip4 ipa)
		{
			this->ip = ipa;
			type_ = AT_TCP_RESLOVED;
		}

		void set_port(signed_t p)
		{
			this->port_ = p;
		}

		void set_domain(const std::string& dom)
		{
			this->domain_ = dom;
			type_ = AT_TCP_DOMAIN;
		}

		ip4 get_ip4(bool log_enable) const;
		signed_t port() const
		{
			return port_;
		}
		addr_type type() const
		{
			return type_;
		}
		std::string domain() const
		{
			return domain_;
		}
		std::string desc() const
		{
			std::string d(domain_);
			if (!d.empty())
			{
				d.push_back(':'); d.append(std::to_string(port_));
			}
			switch (type_)
			{
			case netkit::AT_TCP_DOMAIN:
				break;
			case netkit::AT_TCP_RESLOVED:
				if (d.empty())
				{
					d = ip.to_string();
					d.push_back(':');
					d.append(std::to_string(port_));
				}
				else
				{
					std::string ipa = ip.to_string();
					if (d.find(ipa) == d.npos)
					{
						d.append(ASTR(" ("));
						d.append(ipa);
						d.push_back(')');
					}
				}
				break;
			default:
				if (!d.empty()) d.push_back(' ');
				d.append(ASTR("(unresolved)"));
				break;
			}
			return d;
		}
	};

	struct tcp_pipe;
	struct socket
	{
		std::string name;
#ifdef _WIN32
		waitable_data _socket;
		SOCKET sock() const { return _socket.s; }
		WAITABLE get_waitable()
		{
			if (_socket.wsaevent == nullptr)
			{
				_socket.wsaevent = WSACreateEvent();
				WSAEventSelect(_socket.s, _socket.wsaevent, FD_READ|FD_CLOSE);
			}
			return &_socket;
		}
#else
		SOCKET _socket = INVALID_SOCKET;
		SOCKET sock() const { return _socket; }
#endif

		void close(bool flush_before_close);
		bool ready() const { return sock() != INVALID_SOCKET; }
		virtual ~socket() { close(false); }

		bool tcp_listen(const ip4& bind2, int port);
		tcp_pipe* tcp_accept();
	};

	struct pipe : public ptr::sync_shared_object
	{
		enum sendrslt
		{
			SEND_OK,
			SEND_FAIL,
			SEND_BUFFERFULL,
		};

		pipe() {}
		virtual ~pipe() {}

		pipe(const tcp_pipe&) = delete;
		pipe(tcp_pipe&&) = delete;

		virtual sendrslt send(const u8* data, signed_t datasize) = 0;
		virtual signed_t recv(u8* data, signed_t maxdatasz) = 0;
		virtual WAITABLE get_waitable() = 0;
		virtual void close(bool flush_before_close) = 0;
	};

	using pipe_ptr = ptr::shared_ptr<pipe>;

	struct tcp_pipe : public pipe, public socket
	{
		sockaddr_in addr = {};
		signed_t creationtime = 0;

		tools::circular_buffer<65536 * 2> rcvbuf;
		tools::chunk_buffer<16384> outbuf;

		tcp_pipe() { creationtime = chrono::ms(); }
		tcp_pipe(SOCKET s, const sockaddr_in& addr) :addr(addr) { _socket = s; creationtime = chrono::ms(); }
		tcp_pipe(const tcp_pipe&) = delete;
		tcp_pipe(tcp_pipe&&) = delete;

		~tcp_pipe()
		{
		}

		/*virtual*/ void close(bool flush_before_close) override
		{
			rcvbuf.clear();
			socket::close(flush_before_close);
		}

		bool timeout() const
		{
			return (chrono::ms() - creationtime) > 10000; // 10 sec
		}

		void set_address(ip4 IPv4, signed_t port)
		{
			addr.sin_family = AF_INET;
			addr.sin_addr = IPv4;
			addr.sin_port = netkit::to_ne((u16)port);
		}
		void set_address(const endpoint & ainf)
		{
			addr.sin_family = AF_INET;
			addr.sin_addr = ainf.get_ip4(false);
			addr.sin_port = netkit::to_ne((u16)ainf.port());
		}

		bool connect();

		tcp_pipe& operator=(tcp_pipe&& p)
		{
			if (connected())
				close(false);

			_socket = p._socket; p._socket = INVALID_SOCKET;
			addr = p.addr;
			rcvbuf = std::move(p.rcvbuf);
			return *this;
		}

		bool connected() const { return ready(); }

		/*virtual*/ sendrslt send(const u8* data, signed_t datasize) override;
		/*virtual*/ signed_t recv(u8* data, signed_t maxdatasz) override;

		enum read_result : u8
		{
			OK,
			NOT_YET_READY,
			DEAD
		};

		template<signed_t N> using ret_t = std::tuple<vbv<N>, read_result>;

		/*
		template<signed_t N> ret_t<N> read(signed_t shift)
		{
			rcv_all();
			if (rcvbuf.datasize() >= N + shift)
			{
				return { vbv<N>(rcvbuf + shift), OK };
			}
			return { vbv<N>(), connected() ? NOT_YET_READY : DEAD };
		}
		*/


		bool rcv_all(); // receive all, but stops when buffer size reaches 64k
		/*virtual*/ WAITABLE get_waitable() override;

		/*
		void cpdone(signed_t psz) // current packet done
		{
			if (rcvbuf_sz == psz)
				rcvbuf_sz = 0;
			else if (rcvbuf_sz > psz)
			{
				rcvbuf_sz -= psz;
				memcpy(rcvbuf, rcvbuf + psz, rcvbuf_sz);
			}
		}
		*/
	};



	bool dnsresolve(const std::string& host, ip4& addr);



	class pgen
	{
		u8* data;
		signed_t ptr = 0;
	public:
		signed_t sz;
		pgen(u8* data, signed_t sz) :data(data), sz(sz) {}
		~pgen() {
			ASSERT(ptr == sz);
		}

		void push8(signed_t b)
		{
			data[ptr++] = (u8)b;
		}
		void push16(signed_t b) // push in big endiang order
		{
			data[ptr++] = (u8)((b >> 8) & 0xff); // high first
			data[ptr++] = (u8)((b) & 0xff); // low second
		}
		void pushs(const std::string& s)
		{
			data[ptr++] = (u8)s.length();
			memcpy(data + ptr, s.c_str(), s.length());
			ptr += s.length();
		}
		void push(netkit::ip4 ip) // push ip
		{
			memcpy(data + ptr, &ip, 4);
			ptr += 4;
		}
	};

}

