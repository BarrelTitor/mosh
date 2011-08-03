#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>

#include "dos_assert.hpp"
#include "network.hpp"

using namespace std;
using namespace Network;

template <class Payload>
Flow<Payload>::Packet::DecodingCache::DecodingCache( string coded_packet )
  : direction( TO_CLIENT ), seq( -1 ), payload_string()
{
  dos_assert( coded_packet.size() >= 8 );

  /* Read in sequence number and direction */
  string seq_string( coded_packet.begin(), coded_packet.begin() + 8 );
  uint64_t *network_order_seq = (uint64_t *)seq_string.data();
  uint64_t direction_seq = be64toh( *network_order_seq );
  direction = (direction_seq & 8000000000000000) ? TO_CLIENT : TO_SERVER;
  seq = direction_seq & 0x7FFFFFFFFFFFFFFF;

  /* Read in payload */
  payload_string = string( coded_packet.begin() + 8, coded_packet.end() );  
}

template <class Payload>
Flow<Payload>::Packet::Packet( string coded_packet )
  : decoding_cache( coded_packet ),
    seq( decoding_cache.seq ),
    direction( decoding_cache.direction ),
    payload( decoding_cache.payload_string )
{}

template <class Payload>
string Flow<Payload>::Packet::tostring( void )
{
  uint64_t direction_seq = ((uint64_t( direction == TO_CLIENT ) & 0x1) << 63) | (seq & 0x7FFFFFFFFFFFFFFF);
  uint64_t network_order_seq = htobe64( direction_seq );
  const char *seq_str = (const char *)&network_order_seq;
  string seq_string( seq_str, 8 ); /* necessary in case there is a zero byte */

  return seq_string + payload.tostring();
}

template <class Payload>
typename Flow<Payload>::Packet Flow<Payload>::new_packet( Payload &s_payload )
{
  return Packet( next_seq++, direction, s_payload );
}

template <class Outgoing, class Incoming>
Connection<Outgoing, Incoming>::Connection( bool s_server )
  : flow( s_server ? TO_CLIENT : TO_SERVER ),
    sock( -1 ),
    remote_addr(),
    server( s_server ),
    attached( false )
{

  /* create socket */
  sock = socket( AF_INET, SOCK_DGRAM, 0 );
  if ( sock < 0 ) {
    perror( "socket" );
    exit( 1 );
  }

  /* Bind to free local port.
     This usage does not seem to be endorsed by POSIX. */
  struct sockaddr_in local_addr;
  local_addr.sin_family = AF_INET;
  local_addr.sin_port = htons( 0 );
  local_addr.sin_addr.s_addr = INADDR_ANY;
  if ( bind( sock, (sockaddr *)&local_addr, sizeof( local_addr ) ) < 0 ) {
    perror( "bind" );
    exit( 1 );
  }
}

template <class Outgoing, class Incoming>
void Connection<Outgoing, Incoming>::client_connect( const char *ip, int port )
{
  assert( !server );

  /* associate socket with remote host and port */
  remote_addr.sin_family = AF_INET;
  remote_addr.sin_port = htons( port );
  if ( !inet_aton( ip, &remote_addr.sin_addr ) ) {
    fprintf( stderr, "Bad IP address %s\n", ip );
    exit( 1 );
  }

  if ( connect( sock, (sockaddr *)&remote_addr, sizeof( remote_addr ) ) < 0 ) {
    perror( "connect" );
    exit( 1 );
  }

  attached = true;
}

template <class Outgoing, class Incoming>
void Connection<Outgoing, Incoming>::send( Outgoing &s )
{
  assert( attached );

  string p = flow.new_packet( s ).tostring();

  if ( sendto( sock, p.data(), p.size(), 0,
	       (sockaddr *)&remote_addr, sizeof( remote_addr ) ) < 0 ) {
    perror( "sendto" );
    exit( 1 );
  }
}

template <class Outgoing, class Incoming>
Incoming Connection<Outgoing, Incoming>::recv( void )
{
  struct sockaddr_in packet_remote_addr;

  char buf[ RECEIVE_MTU ];

  socklen_t addrlen = sizeof( packet_remote_addr );

  ssize_t received_len = recvfrom( sock, buf, RECEIVE_MTU, 0, (sockaddr *)&packet_remote_addr, &addrlen );

  if ( received_len < 0 ) {
    perror( "recvfrom" );
    exit( 1 );
  }

  if ( received_len > RECEIVE_MTU ) {
    fprintf( stderr, "Received oversize datagram (size %d) and limit is %d.\n",
	     static_cast<int>( received_len ), RECEIVE_MTU );
    exit( 1 );
  }

  typename Flow<Incoming>::Packet p( string( buf, received_len ) );
  dos_assert( p.direction == (server ? TO_SERVER : TO_CLIENT) ); /* prevent malicious playback to sender */

  /* server auto-adjusts to client */
  if ( server ) {
    attached = true;

    if ( (remote_addr.sin_addr.s_addr != packet_remote_addr.sin_addr.s_addr)
	 || (remote_addr.sin_port != packet_remote_addr.sin_port) ) {
      remote_addr = packet_remote_addr;
      fprintf( stderr, "Server now attached to client at %s:%d\n",
	       inet_ntoa( remote_addr.sin_addr ),
	       ntohs( remote_addr.sin_port ) );
    }
  }

  return p.payload;
}

template <class Outgoing, class Incoming>
int Connection<Outgoing, Incoming>::port( void )
{
  struct sockaddr_in local_addr;
  socklen_t addrlen = sizeof( local_addr );

  if ( getsockname( sock, (sockaddr *)&local_addr, &addrlen ) < 0 ) {
    perror( "getsockname" );
    exit( 1 );
  }

  return ntohs( local_addr.sin_port );
}