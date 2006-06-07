/*
    This file is part of Icecream.

    Copyright (c) 2004 Stephan Kulow <coolo@suse.de>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*/

#include <config.h>
#include "environment.h"
#include <logging.h>
#include <errno.h>
#include <dirent.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#ifdef __FreeBSD__
#include <signal.h>
#endif

#include "comm.h"

using namespace std;

extern bool maybe_stats(bool forced);

#if 0
static string read_fromFILE( FILE *f )
{
    string output;
    if ( !f ) {
        log_error() << "no pipe " << strerror( errno ) << endl;
        return output;
    }
    char buffer[100];
    while ( !feof( f ) ) {
        size_t bytes = fread( buffer, 1, 99, f );
        buffer[bytes] = 0;
        output += buffer;
    }
    pclose( f );
    return output;
}

static bool extract_version( string &version )
{
    string::size_type pos = version.find_last_of( '\n' );
    if ( pos == string::npos )
        return false;

    while ( pos + 1 == version.size() ) {
        version.resize( version.size() - 1 );
        pos = version.find_last_of( '\n' );
        if ( pos == string::npos )
            return false;
    }

    version = version.substr( pos + 1);
    return true;
}
#endif

static size_t sumup_dir( const string &dir )
{
    size_t res = 0;
    DIR *envdir = opendir( dir.c_str() );
    if ( !envdir )
        return res;

    struct stat st;
    string tdir = dir + "/";

    for ( struct dirent *ent = readdir(envdir); ent; ent = readdir( envdir ) )
    {
        if ( !strcmp( ent->d_name, "." ) || !strcmp( ent->d_name, ".." ) )
            continue;

        if ( lstat( ( tdir + ent->d_name ).c_str(), &st ) ) {
            perror( "stat" );
            continue;
        }

        if ( S_ISDIR( st.st_mode ) )
            res += sumup_dir( tdir + ent->d_name );
        else if ( S_ISREG( st.st_mode ) )
            res += st.st_size;
        // else ignore
    }
    closedir( envdir );
    return res;
}

static string list_native_environment( const string &nativedir )
{
    assert( nativedir.at( nativedir.length() - 1 ) == '/' );

    string native_environment;

    DIR *tdir = opendir( nativedir.c_str() );
    if ( tdir ) {
        string suff = ".tar.gz";
        do {
            struct dirent *myenv = readdir(tdir);
            if ( !myenv )
                break;
            string versfile = myenv->d_name;
            if ( versfile.size() > suff.size() && versfile.substr( versfile.size() - suff.size() ) == suff ) {
                native_environment = nativedir + versfile;
                break;
            }
        } while ( true );
        closedir( tdir );
    }
    return native_environment;
}

static void list_target_dirs( const string &current_target, const string &targetdir, Environments &envs )
{
    DIR *envdir = opendir( targetdir.c_str() );
    if ( !envdir )
        return;

    for ( struct dirent *ent = readdir(envdir); ent; ent = readdir( envdir ) )
    {
        string dirname = ent->d_name;
        if ( !access( string( targetdir + "/" + dirname + "/usr/bin/gcc" ).c_str(), X_OK ) )
            envs.push_back( make_pair( current_target, dirname ) );
    }
    closedir( envdir );
}

bool cleanup_cache( const string &basedir, uid_t nobody_uid, gid_t nobody_gid )
{
    pid_t pid = fork();
    if ( pid )
    {
        int status = 0;
        while ( waitpid( pid, &status, 0 ) < 0 && errno == EINTR )
            ;
        return WIFEXITED(status);
    }
    // else
    if ( setgid( nobody_gid ) < 0 ) {
      log_perror("setgid failed");
      _exit (143);
    }

    if (!geteuid() && setuid( nobody_uid ) < 0) {
      log_perror("setuid failed");
      _exit (142);
    }

    if ( !::access( basedir.c_str(), W_OK ) ) { // it exists - removing content

        if ( chdir( basedir.c_str() ) ) {
            log_error() << "chdir" << strerror( errno ) << endl;
            _exit( 1 );
        }

        char buffer[PATH_MAX];
        DIR *dir = opendir( "." );
        if ( !dir )
            _exit( 1 ); // very unlikely

        struct dirent *subdir = readdir( dir );
        while ( subdir ) {
            if ( !strcmp( subdir->d_name, "." ) || !strcmp( subdir->d_name, ".." ) ) {
                subdir = readdir( dir );
                continue;
            }
            snprintf( buffer, PATH_MAX, "rm -rf '%s'", subdir->d_name );
            if ( system( buffer ) ) {
                log_error() << "rm -rf failed\n";
                _exit( 1 );
            }
            subdir = readdir( dir );
        }
        closedir( dir );
    }

    if ( mkdir( basedir.c_str(), 0755 ) && errno != EEXIST ) {
        if ( errno == EPERM )
            log_error() << "cache directory can't be generated: " << basedir << endl;
        else
            log_perror( "Failed " );
        _exit( 1 );
    }
    _exit( 0 );
}

Environments available_environmnents(const string &basedir)
{
    Environments envs;

    DIR *envdir = opendir( basedir.c_str() );
    if ( !envdir ) {
        log_info() << "can't open envs dir " << strerror( errno ) << endl;
    } else {
        for ( struct dirent *target_ent = readdir(envdir); target_ent; target_ent = readdir( envdir ) )
        {
            string dirname = target_ent->d_name;
            if ( dirname.at( 0 ) == '.' )
                continue;
            if ( dirname.substr( 0, 7 ) == "target=" )
            {
                string current_target = dirname.substr( 7, dirname.length() - 7 );
                list_target_dirs( current_target, basedir + "/" + dirname, envs );
            }
        }
        closedir( envdir );
    }

    return envs;
}

size_t setup_env_cache(const string &basedir, string &native_environment, uid_t nobody_uid, gid_t nobody_gid)
{
    native_environment = "";
    string nativedir = basedir + "/native/";

    pid_t pid = fork();
    if ( pid )
    {
        int status = 0;
        if ( waitpid( pid, &status, 0 ) != pid )
            status = 1;
        trace() << "waitpid " << status << endl;
        if ( !status )
        {
            trace() << "opendir " << nativedir << endl;
            native_environment = list_native_environment( nativedir );
            if ( native_environment.empty() )
                status = 1;
        }
        trace() << "native_environment " << native_environment << endl;
        if ( status )
            return 0;
        else {
            return sumup_dir( nativedir );
        }
    }
    // else
    if ( setgid( nobody_gid ) < 0) {
      log_perror("setgid failed");
      _exit(143);
    }
    if (!geteuid() && setuid( nobody_uid ) < 0) {
      log_perror("setuid failed");
      _exit (142);
    }

    if ( !::access( "/usr/bin/gcc", X_OK ) && !::access( "/usr/bin/g++", X_OK ) ) {
        if ( !mkdir ( nativedir.c_str(), 0755 ) )
        {
            if ( chdir( nativedir.c_str() ) ) {
                log_perror( "chdir" );
                rmdir( nativedir.c_str() );
                goto error;
            }

            if ( system( BINDIR "/create-env" ) ) {
                log_error() << BINDIR "/create-env failed\n";
                goto error;
            } else {
                _exit( 0 );
            }
        }
    }

error:
    rmdir( nativedir.c_str() );
    _exit( 1 );
}

size_t install_environment( const std::string &basename, const std::string &target,
                          const std::string &name, MsgChannel *c, uid_t nobody_uid, gid_t nobody_gid )
{
    if ( !name.size() || name[0] == '.' ) {
        log_error() << "illegal name for environment " << name << endl;
        return 0;
    }

    for ( string::size_type i = 0; i < name.size(); ++i ) {
        if ( isascii( name[i] ) && !isspace( name[i]) && name[i] != '/' && isprint( name[i] ) )
            continue;
        log_error() << "illegal char '" << name[i] << "' - rejecting environment " << name << endl;
        return 0;
    }

    string dirname = basename + "/target=" + target;
    Msg *msg = c->get_msg(30);
    if ( !msg || msg->type != M_FILE_CHUNK )
    {
        trace() << "Expected first file chunk\n";
        return 0;
    }

    FileChunkMsg *fmsg = dynamic_cast<FileChunkMsg*>( msg );
    enum { BZip2, Gzip, None} compression = None;
    if ( fmsg->len > 2 )
    {
        if ( fmsg->buffer[0] == 037 && fmsg->buffer[1] == 0213 )
            compression = Gzip;
        else if ( fmsg->buffer[0] == 'B' && fmsg->buffer[1] == 'Z' )
            compression = BZip2;
    }

    int fds[2];
    if ( pipe( fds ) )
        return 0;

    pid_t pid = fork();
    if ( pid )
    {
        trace() << "pid " << pid << endl;
        close( fds[0] );
        FILE *fpipe = fdopen( fds[1], "w" );

        bool error = false;
        do {

            maybe_stats(false);
            int ret = fwrite( fmsg->buffer, fmsg->len, 1, fpipe );
            if ( ret != 1 ) {
                log_error() << "wrote " << ret << " bytes\n";
                error = true;
                break;
            }
            delete msg;
            msg = c->get_msg(30);
            if (!msg) {
                error = true;
                break;
            }

            if ( msg->type == M_END ) {
                trace() << "end\n";
                break;
            }
            fmsg = dynamic_cast<FileChunkMsg*>( msg );

            if ( !fmsg ) {
                log_error() << "Expected another file chunk\n";
                error = true;
                break;
            }
        } while ( true );

        maybe_stats(false);

        delete msg;

        fclose( fpipe );
        close( fds[1] );

        int status = 0;
        if ( error ) {
            kill( pid, SIGTERM );
            char buffer[PATH_MAX];
            snprintf( buffer, PATH_MAX, "rm -rf '/%s'", dirname.c_str() );
            system( buffer );
            status = 1;
        } else {
            if ( waitpid( pid, &status, 0) != pid )
                status = 1;
            dirname = dirname + "/" + name;
            mkdir( ( dirname + "/var" ).c_str(), 0755 );
            chown( ( dirname + "/var" ).c_str(), nobody_uid, nobody_gid );
            mkdir( ( dirname + "/var/tmp" ).c_str(), 0755 );
            chown( ( dirname + "/var/tmp" ).c_str(), nobody_uid, nobody_gid );
            mkdir( ( dirname + "/tmp" ).c_str(), 01755 );
            chown( ( dirname + "/tmp" ).c_str(), 0, nobody_gid );
            chmod( ( dirname + "/tmp" ).c_str(), 01775 );
        }

        if ( status ) {
            return 0;
        } else {
            return sumup_dir( dirname );
        }
    }
    // else
    if ( setgid( nobody_gid ) < 0) {
      log_perror("setgid fails");
      _exit(143);
    }
    if (!geteuid() && setuid( nobody_uid ) < 0) {
      log_perror("setuid fails");
      _exit (142);
    }

    close( 0 );
    close( fds[1] );
    dup2( fds[0], 0 );

    if( ::access( basename.c_str(), W_OK ) ) {
       log_perror( basename.c_str() );
       _exit( 1 );
    }

    if ( mkdir( dirname.c_str(), 0755 ) && errno != EEXIST ) {
        log_perror( "mkdir target" );
        _exit( 1 );
    }

    dirname = dirname + "/" + name;
    if ( mkdir( dirname.c_str(), 0755 ) ) {
        log_perror( "mkdir name" );
        _exit( 1 );
    }

    if ( chdir( dirname.c_str() ) ) {
        log_perror( "chdir" );
        _exit( 1 );
    }

    char **argv;
    argv = new char*[4];
    argv[0] = strdup( "/bin/tar" );
    if ( compression == BZip2 )
        argv[1] = strdup( "xjf" );
    else if ( compression == Gzip )
        argv[1] = strdup( "xzf" );
    else if ( compression == None )
        argv[1] = strdup( "xf" );
    argv[2] = strdup( "-" );
    argv[3] = 0;
    _exit(execv( argv[0], argv ));
}

size_t remove_environment( const string &basename, const string &env, uid_t nobody_uid, gid_t nobody_gid )
{
    string::size_type pos = env.find_first_of( '/' );
    if ( pos == string::npos ) // nonsense
        return 0;

    string target = env.substr( 0, pos );
    string name = env.substr( pos + 1 );
    string dirname = basename + "/target=" + target;
    trace() << "removing " << dirname << "/" << name << endl;

    size_t res = sumup_dir( dirname + "/" + name );

    pid_t pid = fork();
    if ( pid )
    {
        int status = 0;
        while ( waitpid( pid, &status, 0 ) < 0 && errno == EINTR )
            ;
         if ( WIFEXITED (status) )
             return res;
        // something went wrong. assume no disk space was free'd.
        return 0;
    }
    // else

    if ( chdir( dirname.c_str() ) != 0 ) {
        log_perror( "chdir failed" );
        _exit( 144 );
    }
    if ( setgid(nobody_gid) < 0) {
      log_perror("setgid fails");
      _exit(143);
    }
    if (!geteuid() && setuid( nobody_uid ) < 0) {
      log_perror("setuid fails");
      _exit (142);
    }

    char **argv;
    argv = new char*[4];
    argv[0] = strdup( "/bin/rm" );
    argv[1] = strdup( "-rf" );
    argv[2] = strdup( name.c_str() );
    argv[3] = NULL;

    _exit(execv(argv[0], argv));
}