#include <iostream>
#include <stdio.h>
#include <time.h>
#include <string>
#include <sys/types.h>
#include <sys/stat.h>
#include <assert.h>
#include <set>
#include <list>


#ifdef _WIN32
	#include <errno.h>
	#include <io.h>
	#include <direct.h>
	#include <functional>
	#include <hash_map>
#else
	#include <dirent.h>
	#include <memory>
	#include <ext/functional>
    #if !defined (__CXX_11__)
    	#include <ext/hash_map>
    #else
    	#include <unordered_map>
    #endif
    #include <memory.h>
    #include <unistd.h>
	#define _stricmp strcasecmp
#endif
#include <algorithm>
#include <typeinfo>
#include <math.h>

#include "mytypes.h"
#include "md4.h"
#include "buffer.h"
#include "simple_socket.h"
#include "passive_socket.h"
#include "rw.h"
#include "rollsum.h"
#include "xdeltalib.h"
#include "platform.h"
#include "reconstruct.h"
#include "tinythread.h"
#include "xdelta_server.h"
#include "xdelta_client.h"

class source_observer : public xdelta::xdelta_observer
{
	struct file_stat {
		std::string			fname_;
		xdelta::uint64_t	same_block_nr_;
		xdelta::uint64_t	same_block_bytes_;
		xdelta::uint64_t	diff_block_nr_;
		xdelta::uint64_t	diff_block_bytes_;
		xdelta::uint64_t	recv_bytes_;
		xdelta::uint64_t	send_bytes_;
		void init() {
			fname_.clear ();
			same_block_nr_ = same_block_bytes_ = diff_block_nr_ =
				diff_block_bytes_ = recv_bytes_ = send_bytes_ = 0;
		}
	};

public:
	virtual void start_hash_stream (const std::string & fname, const xdelta::int32_t blk_len)
	{
		//lock_.lock ();
		stat_.init();
		files_nr_++;
		stat_.fname_ = fname;
	}
	virtual void add_block (xdelta::uint32_t fhash, const xdelta::slow_hash & shash)
	{
	}
	virtual void end_first_round ()
	{
		std::cout << "end first round ...\n";
	}
	virtual void next_round (const xdelta::int32_t blk_len)
	{
		std::cout << "next round " << blk_len << " ...\n";
	}
	virtual void end_one_round ()
	{
		std::cout << "end one round ...\n";
	}
	virtual void end_hash_stream (const xdelta::uint64_t filesize)
	{
		tfilsize_ += filesize;
		printf("file:%s, same block bytes/nr:[%lld,%lld], diff block bytes/nr:[%lld, %lld]\n",
			stat_.fname_.c_str(), stat_.same_block_bytes_, stat_.same_block_nr_
			, stat_.diff_block_bytes_, stat_.diff_block_nr_);
		//lock_.unlock ();
	}
	void on_error (const std::string & errmsg, const int errorno)
	{
		printf ("error:%s(%s)\n", errmsg.c_str (), xdelta::error_msg ().c_str ());
	}
	virtual void bytes_send (const xdelta::uint32_t bytes)
	{
		send_bytes_ += bytes;
		stat_.send_bytes_ += bytes;
	}

	virtual void bytes_recv (const xdelta::uint32_t bytes)
	{
		recv_bytes_ += bytes;
		stat_.recv_bytes_ += bytes;
	}
	virtual void on_equal_block(const xdelta::uint32_t blk_len
		, const xdelta::uint64_t s_offset)
	{
		stat_.same_block_bytes_ += blk_len;
		stat_.same_block_nr_++;
	}
	virtual void on_diff_block(const xdelta::uint32_t blk_len)
	{
		stat_.diff_block_bytes_ += blk_len;
		stat_.diff_block_nr_++;
	}
	xdelta::uint64_t	recv_bytes_;
	xdelta::uint64_t	send_bytes_;
	xdelta::uint64_t	tfilsize_;
	int					files_nr_;
	file_stat			stat_;
	xdelta::mutex		lock_;

	source_observer () : recv_bytes_ (0)
					, send_bytes_ (0)
					, tfilsize_ (0)
					, files_nr_ (0)
	{
		stat_.init();
	}
};

struct my_deletor : public xdelta::deletor
{
	virtual void release (xdelta::file_reader * p) { delete p; }
	virtual void release (xdelta::hasher_stream * p) { delete p; }
	virtual void release (xdelta::xdelta_stream * p) { delete p; }
	virtual void release (xdelta::hash_table * p) { delete p; }
}mydeletor;

void handle_file (const std::string pathname
				, const std::string & fname
				, xdelta::xdelta_client & client)
{
	try {
		client.add_task (new xdelta::f_local_freader (pathname, fname), &mydeletor);
	} catch (xdelta::xdelta_exception &e) {
		if (e.get_errno () != EACCES)
			throw;
	}
}

void traverse_source (const std::string & pathname
						, const std::string & subpath
						, xdelta::xdelta_client & client)
{
	std::string fullpath = pathname;
	if (!subpath.empty ())
		fullpath = pathname + SEPERATOR + subpath;

#ifdef _WIN32
	struct _stat st;
	int ret = _stat(fullpath.c_str (), &st);
#else
	struct stat st;
	int ret = stat(fullpath.c_str (), &st);
#endif
	if (ret < 0) {
		std::string errmsg = xdelta::fmt_string ("Can't not stat file %s(%s)."
										, fullpath.c_str ()
										, xdelta::error_msg ().c_str ());
		printf ("%s\n", errmsg.c_str ());
		return;
	}

#ifdef _WIN32
	if (st.st_mode & _S_IFDIR) {
		struct _finddata_t fi;
		std::string pattern = fullpath + "/*.*";
		intptr_t handle = _findfirst (pattern.c_str (), &fi);
		if (handle != -1) {
			int res;
			while ((res = _findnext (handle, &fi)) == 0) {
				if (strcmp (fi.name, ".") == 0 || strcmp (fi.name, "..") == 0) continue;
				std::string subpathname;
				if (subpath.empty ())
					subpathname = fi.name;
				else
					subpathname = subpath + SEPERATOR + std::string (fi.name);
				traverse_source (pathname, subpathname, client);
			}
			_findclose (handle);
		}
#else
	if (st.st_mode & S_IFDIR) {
		DIR *dir;
		struct dirent *dent;
		dir = opendir(fullpath.c_str ());   //this part
		if(dir!=NULL) {
			while((dent=readdir(dir))!=NULL) {
				if (strcmp (dent->d_name, ".") == 0 || strcmp (dent->d_name, "..") == 0)
					continue;
					
				std::string subpathname;
				if (subpath.empty ())
					subpathname = dent->d_name;
				else
					subpathname = subpath + SEPERATOR + std::string (dent->d_name);
					    
				traverse_source (pathname, subpathname, client);
			}
		}
		closedir(dir);
#endif
	}
	else {
		handle_file (pathname, subpath, client);
	}
}

int main (int argn, char ** argc)
{
	if (argn != 4) {
		return -1;
	}

	try {
		source_observer ob;
		bool compress = _stricmp (argc[2], "z") == 0 ? true : false;
		xdelta::xdelta_client client (compress);

		xdelta::uint64_t start = time (0);
		std::string path (argc[1]);
		xdelta::f_local_creator localop (path);
		xdelta::file_operator &fop = localop;
		client.run (fop, ob, (xdelta::uchar_t*)argc[3]);
		traverse_source (path, "", client);
		client.wait ();
		xdelta::uint64_t end = time (0);
		printf ("TIME:\t %d seconds\nSEND:\t%lld bytes\nRECV:\t%lld bytes\nRATIO:\t%.05f\nAVERAGE: %lld per second.\n"
			, (int)(end - start),  ob.send_bytes_, ob.recv_bytes_
			, (float)((ob.send_bytes_ + ob.recv_bytes_)/(float)(ob.tfilsize_))
			, (xdelta::uint64_t)((ob.send_bytes_ + ob.recv_bytes_ + ob.tfilsize_)/(float)(end - start)));
	}
	catch (xdelta::xdelta_exception &e) {
		printf ("%s\n", e.what ());
#ifdef _WIN32
		system ("pause");
#endif
		return -1;
	}
#ifdef _WIN32
	system ("pause");
#endif
    return 0;
}
