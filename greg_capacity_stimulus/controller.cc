#include <iostream>
#include <deque>
#include <sstream>
#include <iomanip>
#include <cassert>
#include <algorithm>

#include "controller.hh"
#include "timestamp.hh"

#define MIN_WINDOW_SIZE 2
#define MAX_REORDER_MS 20

using namespace std;

/* Default constructor */
Controller::Controller( const bool debug )
  : debug_( debug )
  , outstanding_datagrams()
  , max_packets_in_flight(MIN_WINDOW_SIZE)
  , timestamp_window_last_changed(0)
  , rtt_ewma(100)
  , min_rtt_seen(1000)
  , short_term_loss_ewma(0)
  , long_term_loss_ewma(0)
  , log_()
{
    time_t t = time(nullptr);
    struct tm *now = localtime(&t);

    if (debug_) {
      char buffer[80];
      strftime(buffer, sizeof(buffer),
               "greg-saturator-%Y-%m-%dT%H-%M-%S.log", now);
      string filename(buffer);
      cerr << "Log saved to " + filename << endl;

      log_.reset(new ofstream(filename));
    }
}

/* Payload size of every datagram */
unsigned int Controller::payload_size(void)
{
  return 1388;
}

bool Controller::window_is_open( void )
{
  return outstanding_datagrams.size() < max_packets_in_flight;
}

  /* Set the period in ms of timeout timer (return 0 to disable timer) */
unsigned int Controller::timer_period(void)
{
  return 0;
}

/* Timeout timer fires */
void Controller::timer_fires(void)
{
}

/* A datagram was sent */
void Controller::datagram_was_sent( const uint64_t sequence_number,
				    /* of the sent datagram */
				    const uint64_t send_timestamp )
                                    /* in milliseconds */
{
  outstanding_datagrams.emplace_back( sequence_number, send_timestamp );

  if ( debug_ ) {
    cerr << "At time " << send_timestamp
	 << " sent datagram " << sequence_number
        << " window size is " << outstanding_datagrams.size() << endl;
  }
}

/* An ack was received */
void Controller::ack_received( const uint64_t sequence_number_acked,
			       /* what sequence number was acknowledged */
			       const uint64_t send_timestamp_acked,
			       /* when the acknowledged datagram was sent (sender's clock) */
			       const uint64_t recv_timestamp_acked,
			       /* when the acknowledged datagram was received (receiver's clock)*/
			       const uint64_t timestamp_ack_received )
                               /* when the ack was received (by sender) */
{
    /* Write RTTs to log */
    if (debug_) {
      *log_ << timestamp_ack_received - send_timestamp_acked << endl;
    }

    const double short_term_ewma_factor = .05;
    const double long_term_ewma_factor = .001;
    while ( ( outstanding_datagrams.front().second + MAX_REORDER_MS ) < send_timestamp_acked ) {
        outstanding_datagrams.pop_front();
        short_term_loss_ewma = 1. * short_term_ewma_factor + ( 1 - short_term_ewma_factor ) * short_term_loss_ewma;
        long_term_loss_ewma = 1. * long_term_ewma_factor + ( 1 - long_term_ewma_factor ) * long_term_loss_ewma;
    }

    uint64_t previous_sequence_number = 0;

    auto it = outstanding_datagrams.begin();
    while (it != outstanding_datagrams.end()) {
        if (it->first == sequence_number_acked) {
          it = outstanding_datagrams.erase(it);
          // for the packet that made it
          short_term_loss_ewma = 0. * short_term_ewma_factor + ( 1 - short_term_ewma_factor ) * short_term_loss_ewma;
          long_term_loss_ewma = 0. * long_term_ewma_factor + ( 1 - long_term_ewma_factor ) * long_term_loss_ewma;
          break;
        } else if (it->first > sequence_number_acked) {
          // ack for packet we already considered lost
          break;
        } else {
          // sanity check sequence numbers monotonic increasing in deque
          assert( previous_sequence_number < it->first );
          previous_sequence_number = it->first;

          it++;
        }
    }

    const double rtt_ewma_factor = .05;
    uint64_t rtt = timestamp_ack_received - send_timestamp_acked;
    min_rtt_seen = min( rtt, min_rtt_seen );
    rtt_ewma = (double) rtt * rtt_ewma_factor + ( 1 - rtt_ewma_factor ) * rtt_ewma;

    //cout << "[";
    //for ( double increment = 0; increment < 1.; increment+= .01 ) {
    //    if (increment < long_term_loss_ewma)
    //        cout << "|";
    //    else
    //        cout << " ";
    //}
    //cout << "]";// << endl;

    // short term changes
    if (timestamp_window_last_changed + 20 < timestamp_ack_received) {
        if ( short_term_loss_ewma < .1 and long_term_loss_ewma < .05) {
            if ( rtt_ewma < (min_rtt_seen + 20 ) ) {
                max_packets_in_flight++;
                max_packets_in_flight++;
                timestamp_window_last_changed = timestamp_ack_received;
            } else if ( rtt_ewma < (min_rtt_seen + 100 ) ) {
                max_packets_in_flight++;
                timestamp_window_last_changed = timestamp_ack_received;
            }
        } else {
            // don't change window
        }
    }

    // longer term changes
    if ( timestamp_window_last_changed + 200 < timestamp_ack_received ) {
        if ( short_term_loss_ewma > .3 and long_term_loss_ewma > .25 ) {
            max_packets_in_flight--;
            timestamp_window_last_changed = timestamp_ack_received;
        } if ( short_term_loss_ewma < .5 and long_term_loss_ewma < .1 ) {
            if ( rtt_ewma < (min_rtt_seen + 500 ) ) {
                max_packets_in_flight++;
                timestamp_window_last_changed = timestamp_ack_received;
            }
        } else if ( rtt_ewma > (min_rtt_seen + 1000 ) ) {
            max_packets_in_flight--;
            timestamp_window_last_changed = timestamp_ack_received;
        }
    }

    max_packets_in_flight = max( max_packets_in_flight, (size_t) MIN_WINDOW_SIZE );

    if ( debug_ ) {
        cerr << "At time " << timestamp_ack_received
            << " received ack for datagram " << sequence_number_acked
            << " (send @ time " << send_timestamp_acked
            << ", received @ time " << recv_timestamp_acked << " by receiver's clock)"
            << rtt
            << endl;
    }
}

/* How long to wait (in milliseconds) if there are no acks
   before sending one more datagram */
int Controller::timeout_ms( void )
{
  return 1500;
}
