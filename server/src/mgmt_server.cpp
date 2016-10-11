#include "mgmt_server.h"

#include "node_cfg_providers/yeti_cfg.h"
#include "node_cfg_providers/lnp_cfg.h"

#include <map>
#include <list>

#include "cfg.h"

std::unique_ptr<mgmt_server> mgmt_server::_self(nullptr);

#include "opts/opts.h"

class daemon_cfg_reader: public cfg_reader {
  public:
	daemon_cfg_reader():
		cfg_reader(daemon_opts)
	{}

	bool apply() {
		cfg_t *s;

		//daemon section
		s = cfg_getsec(_cfg,"daemon");
		for(int i = 0; i < cfg_size(s, "listen"); i++)
			cfg.bind_urls.push_back(cfg_getnstr(s, "listen", i));

		log_level = cfg_getint(s,"log_level");
		if(log_level < L_ERR) log_level = L_ERR;
		if(log_level > L_DBG) log_level = L_DBG;

		return true;
	}
};

class system_cfg_reader: public cfg_reader {
  public:
	system_cfg_reader():
		cfg_reader(system_opts)
	{}
};

mgmt_server::mgmt_server():
	_stop(false)
{ }

template<class T>
void add_provider(mgmt_server::cfg_providers_t &p, const char *name, cfg_t *cfg) {
	T *c = new T();
	try {
		c->configure(cfg);
	} catch(...){
		info("leave provider '%s' unconfigured",name);
		delete c;
		return;
	}
	p.insert(std::make_pair(name, c));
}

void mgmt_server::configure()
{
#define add_provider(name,type) add_provider<type>(tmp_cfg_providers,name,cfg)

	cfg_t *cfg;
	cfg_providers_t tmp_cfg_providers;
	std::pair<cfg_providers_t::iterator,bool> i;

	dbg_func();

	daemon_cfg_reader cr;
	if(!cr.load("/etc/yeti/management.cfg")){
		throw std::string("can't load daemon config");
	}

	if(!cr.apply()){
		throw std::string("can't apply daemon config");
	}

	system_cfg_reader scr;
	if(!scr.load("/etc/yeti/system.cfg")){
		throw std::string("can't load system config");
	}
	cfg = scr.get_cfg();

	add_provider("signalling",yeti_cfg_provider);
	add_provider("lnp",lnp_cfg_provider);

	if(tmp_cfg_providers.empty()){
		throw std::string("there are no any configured providers");
	}

	cfg_mutex.lock();
	cfg_providers.swap(tmp_cfg_providers);
	cfg_mutex.unlock();

#undef add_provider
}

void mgmt_server::loop(const std::list<string> &urls)
{
	dbg_func();

	s = nn_socket(AF_SP,NN_REP);
	if(s < 0){
		throw std::string("nn_socket() = %d",s);
	}

	bool binded = false;
	if(cfg.bind_urls.empty()){
		throw std::string("no listen endpoints specified. check your config");
	}

	for(const auto &i : cfg.bind_urls) {
		const char *url = i.c_str();
		int ret = nn_bind(s, url);
		if(ret < 0){
			err("can't bind to url '%s': %d (%s)",url,
				 errno,nn_strerror(errno));
			continue;
		}
		binded = true;
		info("listen on %s",url);
	}

	if(!binded){
		err("there are no listened interfaces after bind. check log and your config");
		throw std::string("can't bind to any url");
	}

	while(!_stop){
		char *msg = NULL;
		int l = nn_recv(s, &msg, NN_MSG, 0);
		if(l < 0){
			if(errno==EINTR) continue;
			dbg("nn_recv() = %d, errno = %d(%s)",l,errno,nn_strerror(errno));
			//!TODO: handle timeout, etc
			continue;
		}
		process_peer(msg,l);
		nn_freemsg(msg);
	}
}

int mgmt_server::process_peer(char *msg, int len)
{
	string reply;
	try {
		CfgRequest req;
		if(!req.ParseFromArray(msg,len)){
			throw std::string("can't decode request");
		}
		create_reply(reply,req);
	} catch(internal_exception &e){
		err("internal_exception: %d %s",e.c,e.e.c_str());
		create_error_reply(reply,e.c,e.e);
	} catch(std::string &e){
		err("%s",e.c_str());
		create_error_reply(reply,500,"Internal Error");
	}

	int l = nn_send(s, reply.data(), reply.size(), 0);
	if(l!=reply.size()){
		err("nn_send(): %d, while msg to send size was %ld",l,reply.size());
	}
	return 0;
}

void mgmt_server::create_error_reply(string &reply,
									 int code, std::string description)
{
	dbg("reply with error: %d %s",code,description.c_str());
	CfgResponse m;
	CfgResponse_Error *e = m.mutable_error();
	if(!e){
		dbg("can't mutate to error oneOf");
		reply.clear();
	}

	e->set_code(code);
	e->set_reason(description);
	m.SerializeToString(&reply);
}

void mgmt_server::create_reply(string &reply, const CfgRequest &req)
{
	info("process request for '%s' node %d",req.cfg_part().c_str(),req.node_id());

	CfgResponse m;
	CfgResponse_Values *v = m.mutable_values();
	if(!v){
		throw internal_exception(500,"serialization error");
	}

	cfg_mutex.lock();

	//temporary hack for back-compatibiltiy (process 'sig_yeti' as 'signalling' )
	const string cfg_part = req.cfg_part()=="sig_yeti"?"signalling": req.cfg_part();

	//check for provider existence
	cfg_providers_t::const_iterator cfg_provider = cfg_providers.find(cfg_part);
	if(cfg_provider==cfg_providers.end()){
		cfg_mutex.unlock();
		throw internal_exception(404,"unknown cfg part");
	}

	//get and serialize config from appropriate config provider
	try {
		cfg_provider->second->serialize(v,req.node_id());
	} catch(cfg_provider::internal_exception &e){
		cfg_mutex.unlock();
		throw internal_exception(e.c,e.e);
	}

	cfg_mutex.unlock();

	m.SerializeToString(&reply);
}

void mgmt_server::show_config(){
	cfg_mutex.lock();
	info("dump runtime configuration");
	for(const auto &i : cfg_providers){
		//info("  %s",i.first.c_str());
		i.second->show_config();
	}
	cfg_mutex.unlock();
}
