#include <unistd.h>
#include <string>
#include <assert.h>
#include <list>

#include "sproutconn.h"
#include "select.h"
#include <sys/time.h>
#include "tapdevice.hh"

using namespace std;
using namespace Network;

int main( int argc, char *argv[] )
{
  char *ip;
  int port;

  Network::SproutConnection *net;

  bool server = true;

  /* Connect to tap0 and setup Sprout tunnel */
  int tap_fd = setup_tap();

  /* Queue incoming packets from tap0 */
  std::queue<string> ingress_queue;

  if ( argc > 1 ) {
    /* client */

    server = false;

    ip = argv[ 1 ];
    port = atoi( argv[ 2 ] );

    net = new Network::SproutConnection( "4h/Td1v//4jkYhqhLGgegw", ip, port );
  } else {
    net = new Network::SproutConnection( NULL, NULL );
  }

  fprintf( stderr, "Port bound is %d\n", net->port() );

  Select &sel = Select::get_instance();
  sel.add_fd( net->fd() );
  sel.add_fd( tap_fd );

  const int fallback_interval = 50;

  /* wait to get attached */
  if ( server ) {
    while ( 1 ) {
      int active_fds = sel.select( -1 );
      if ( active_fds < 0 ) {
	perror( "select" );
	exit( 1 );
      }

      if ( sel.read( net->fd() ) ) {
	net->recv();
      }

      if ( net->get_has_remote_addr() ) {
	break;
      }
    }
  }

  uint64_t time_of_next_transmission = timestamp() + fallback_interval;

  fprintf( stderr, "Looping...\n" );  
  unsigned long int cumulative_bytes = 0;

  /* loop */
  while ( 1 ) {
    int bytes_to_send = net->window_size();
    bool sent = false;

    while ( (bytes_to_send > 0) && (!ingress_queue.empty()) ) {
      /* close window */
      string packet_to_send( ingress_queue.front() );
      ingress_queue.pop();
      bytes_to_send -= packet_to_send.size();

      int time_to_next = 0;
      if ( ingress_queue.empty() || (bytes_to_send <= 0) ) {
	time_to_next = fallback_interval;
      }

      net->send( packet_to_send, time_to_next);
      sent = true;
    }

    if ( (!sent) && (time_of_next_transmission <= timestamp()) ) {
      /* send fallback packet */
      net->send( "", fallback_interval );
      sent = true;
    }

    if ( sent ) {
      time_of_next_transmission = std::max( timestamp() + fallback_interval,
					    time_of_next_transmission );
    }

    /* wait */
    int wait_time = time_of_next_transmission - timestamp();
    if ( wait_time < 0 ) {
      wait_time = 0;
    } else if ( wait_time > 10 ) {
      wait_time = 10;
    }

    int active_fds = sel.select( wait_time );
    if ( active_fds < 0 ) {
      perror( "select" );
      exit( 1 );
    }

    /* receive */
    struct timeval tv;
    if ( sel.read( net->fd() ) ) {
      string packet( net->recv() );

      /* write into tap0 */
      write( tap_fd, packet.c_str(), packet.size() );

      gettimeofday( &tv, NULL );
      cumulative_bytes += packet.size();
      fprintf( stderr, "Rx packet : %lu bytes, cumulative : %lu bytes at time %f \n", packet.size(), cumulative_bytes, (double) tv.tv_sec+tv.tv_usec/1.0e6 );
    }

    /* read from tap0 */
    if ( sel.read( tap_fd ) ) {
      char buffer[1600];
      int nread = read( tap_fd, (void*) buffer, sizeof(buffer) );
      string packet( buffer, nread );
      if ( ingress_queue.size() * 1440 < (int)net->window_predict( 40 ) ) {
	ingress_queue.push( packet );
      }
    }
  }
}
