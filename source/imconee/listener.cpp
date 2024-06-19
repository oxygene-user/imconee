#include "pch.h"

listener* listener::build(loader &ldr, const std::string& name, const asts& bb)
{
	std::string t = bb.get_string(ASTR("type"));
	if (t.empty())
	{
		ldr.exit_code = EXIT_FAIL_TYPE_UNDEFINED;
		LOG_E("{type} not defined for lisnener [%s]. Type {imconee help listener} for more information.", str::printable(name));
		return nullptr;
	}

	if (ASTR("tcp") == t)
	{
		tcp_listener *tcpl = new tcp_listener(ldr, name, bb);
		if (ldr.exit_code != EXIT_OK)
		{
			delete tcpl;
			return nullptr;
		}
		return tcpl;
	}

	LOG_E("unknown {type} [%s] for lisnener [%s]. Type {imconee help listener} for more information.", t.c_str(), str::printable(name));
	ldr.exit_code = EXIT_FAIL_TYPE_UNDEFINED;

	return nullptr;
}

listener::listener(loader& /*ldr*/, const std::string& name, const asts& /*bb*/)
{
	this->name = name;
}

tcp_listener::tcp_listener(loader& ldr, const std::string& name, const asts& bb):listener(ldr, name, bb)
{
	signed_t port = bb.get_int(ASTR("port"));
	if (0 == port)
	{
		ldr.exit_code = EXIT_FAIL_PORT_UNDEFINED;
		LOG_E( "port not defined for listener [%s]", str::printable(name));
		return;
	}

	const asts* hnd = bb.get(ASTR("handler"));
	if (nullptr == hnd)
	{
		ldr.exit_code = EXIT_FAIL_NOHANDLER;
		LOG_E("handler not defined for listener [%s]", str::printable(name));
		return;
	}

	handler* h = handler::build(ldr, this, *hnd);
	if (h == nullptr)
		return; // no warning message here due it generated by handler::build

	hand.reset(h);

	std::string bs = bb.get_string(ASTR("bind"));
	netkit::ip4 bindaddr = netkit::ip4::parse(bs);
	prepare(bindaddr, port);

}


void tcp_listener::acceptor()
{
	auto ss = state.lock_write();
	ss().stage = ACCEPTOR_WORKS;
	ss.unlock();


	auto r = state.lock_read();
	netkit::ip4 bind2 = r().bind;
	int port = r().port;
	r.unlock();

	LOG_N("Listener {%s} has been started (bind ip: %s, port: %i)", str::printable(name), bind2.to_string().c_str(), port);

	if (tcp_listen(bind2, port))
	{
		for (; !state.lock_read()().need_stop;)
		{
			netkit::tcp_pipe* pipe = tcp_accept();
			if (nullptr != pipe)
				hand->on_pipe(pipe);
		}

	}

	ss = state.lock_write();
	ss().stage = IDLE;
}

void tcp_listener::prepare(netkit::ip4 bind2, signed_t port)
{
	stop();

	auto ss = state.lock_write();
	ss().bind = bind2;
	ss().port = (u16)port;
}

/*virtual*/ void tcp_listener::open()
{
	auto ss = state.lock_write();
	if (ss().stage != IDLE)
		return;
	ss().stage = ACCEPTOR_START;
	ss.unlock();

	std::thread th(&tcp_listener::acceptor, this);
	th.detach();
}

/*virtual*/ void tcp_listener::stop()
{
	if (state.lock_read()().stage == IDLE)
		return;

	hand->stop();
	state.lock_write()().need_stop = true;

	closesocket(sock());
	_socket = INVALID_SOCKET;

	while (state.lock_read()().stage != IDLE)
		Sleep(100);

	state.lock_write()().need_stop = false;
}