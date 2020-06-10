//  Broker peering simulation (part 1)
//  Prototypes the state flow

#include "czmq.h"

int main (int argc, char *argv [])
{
    //  First argument is this broker's name
    //  Other arguments are our peers' names
    //
    if (argc < 2) {
        printf ("syntax: peering1 me {you}...\n");
        return 0;
    }
    char *self = argv [1];
    printf ("I: preparing broker at %s...\n", self);
    srandom ((unsigned) time (NULL));

    //  Bind state backend to endpoint
    zsock_t *statebe = zsock_new (ZMQ_PUB);
    zsock_bind (statebe, "ipc://%s-state.ipc", self);
    
    //  Connect statefe to all peers
    zsock_t *statefe = zsock_new (ZMQ_SUB);
    zsock_set_subscribe (statefe, "");
    int argn;
    for (argn = 2; argn < argc; argn++) {
        char *peer = argv [argn];
        printf ("I: connecting to state backend at '%s'\n", peer);
        zsock_connect (statefe, "ipc://%s-state.ipc", peer);
    }
    //  .split main loop
    //  The main loop sends out status messages to peers, and collects
    //  status messages back from peers. The zpoller timeout defines
    //  our own heartbeat:
    zpoller_t *poller = zpoller_new (statefe, NULL);

    while (true) {
        //  Poll for activity, or 1 second timeout
        zsock_t *ready = zpoller_wait (poller, 1000 * ZMQ_POLL_MSEC);
        if (zpoller_terminated(poller))
            break;              //  Interrupted

        //  Handle incoming status messages
        if (ready == statefe) {
            char *peer_name = zstr_recv (statefe);
            char *available = zstr_recv (statefe);
            printf ("%s - %s workers free\n", peer_name, available);
            free (peer_name);
            free (available);
        }
        else {
            //  Send random values for worker availability
            zstr_sendm (statebe, self);
            zstr_sendf (statebe, "%d", randof (10));
        }
    }
    zpoller_destroy (&poller);
    zsock_destroy (&statefe);
    zsock_destroy (&statebe);
    return EXIT_SUCCESS;
}
