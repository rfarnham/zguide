//  Broker peering simulation (part 2)
//  Prototypes the request-reply flow

#include "czmq.h"
#define NBR_CLIENTS 10
#define NBR_WORKERS 3
#define WORKER_READY   "\001"      //  Signals worker is ready

//  Our own name; in practice this would be configured per node
static char *self;

//  .split client task
//  The client task does a request-reply dialog using a standard
//  synchronous REQ socket:

static void
client_task (zsock_t *pipe, void *args)
{
    zsock_signal (pipe, 0);
    zsock_t *client = zsock_new (ZMQ_REQ);
    zsock_connect (client, "ipc://%s-localfe.ipc", self);

    zpoller_t *poller = zpoller_new (pipe, client, NULL);
    zpoller_set_nonstop (poller, 1);

    while (true) {
        //  Send request, get reply
        zstr_send (client, "HELLO");

        zsock_t *ready = zpoller_wait (poller, -1);
        if (ready == pipe)
            break; // Interrupted
        else
            assert(ready == client);

        char *reply = zstr_recv (client);
        if (!reply)
            break;              //  Interrupted
        printf ("Client: %s\n", reply);
        free (reply);
        sleep (1);
    }

    zpoller_destroy (&poller);
    zsock_destroy (&client);
}

//  .split worker task
//  The worker task plugs into the load-balancer using a REQ
//  socket:

static void
worker_task (zsock_t *pipe, void *args)
{
    zsock_signal (pipe, 0);
    zsock_t *worker = zsock_new (ZMQ_REQ);
    zsock_connect (worker, "ipc://%s-localbe.ipc", self);

    //  Tell broker we're ready for work
    zframe_t *frame = zframe_new (WORKER_READY, 1);
    zframe_send (&frame, worker, 0);

    zpoller_t *poller = zpoller_new (pipe, worker, NULL);
    zpoller_set_nonstop (poller, 1);
    //  Process messages as they arrive
    while (true) {
        zsock_t *ready = zpoller_wait (poller, -1);
        if (ready == pipe)
            break;  // Interrupted
        else
            assert (ready == worker);

        zmsg_t *msg = zmsg_recv (worker);
        if (!msg)
            break;              //  Interrupted

        zframe_print (zmsg_last (msg), "Worker: ");
        zframe_reset (zmsg_last (msg), "OK", 2);
        zmsg_send (&msg, worker);
    }
    zpoller_destroy (&poller);
    zsock_destroy (&worker);
}

//  .split main task
//  The main task begins by setting-up its frontend and backend sockets
//  and then starting its client and worker tasks:

int main (int argc, char *argv [])
{
    //  First argument is this broker's name
    //  Other arguments are our peers' names
    //
    if (argc < 2) {
        printf ("syntax: peering2 me {you}...\n");
        return 0;
    }
    self = argv [1];
    printf ("I: preparing broker at %s...\n", self);
    srandom ((unsigned) time (NULL));

    //  Bind cloud frontend to endpoint
    zsock_t *cloudfe = zsock_new (ZMQ_ROUTER);
    zsock_set_identity (cloudfe, self);
    zsock_bind (cloudfe, "ipc://%s-cloud.ipc", self);

    //  Connect cloud backend to all peers
    zsock_t *cloudbe = zsock_new (ZMQ_ROUTER);
    zsock_set_identity (cloudbe, self);
    int argn;
    for (argn = 2; argn < argc; argn++) {
        char *peer = argv [argn];
        printf ("I: connecting to cloud frontend at '%s'\n", peer);
        zsock_connect (cloudbe, "ipc://%s-cloud.ipc", peer);
    }
    //  Prepare local frontend and backend
    zsock_t *localfe = zsock_new (ZMQ_ROUTER);
    zsock_bind (localfe, "ipc://%s-localfe.ipc", self);
    zsock_t *localbe = zsock_new (ZMQ_ROUTER);
    zsock_bind (localbe, "ipc://%s-localbe.ipc", self);

    //  Get user to tell us when we can start...
    printf ("Press Enter when all brokers are started: ");
    getchar ();

    zactor_t *actors[NBR_WORKERS + NBR_CLIENTS];
    int actor_nbr = 0;

    //  Start local workers
    int worker_nbr;
    for (worker_nbr = 0; worker_nbr < NBR_WORKERS; worker_nbr++)
        actors[actor_nbr++] = zactor_new (worker_task, NULL);

    //  Start local clients
    int client_nbr;
    for (client_nbr = 0; client_nbr < NBR_CLIENTS; client_nbr++)
        actors[actor_nbr++] = zactor_new (client_task, NULL);

    // Interesting part
    //  .split request-reply handling
    //  Here, we handle the request-reply flow. We're using load-balancing
    //  to poll workers at all times, and clients only when there are one 
    //  or more workers available.

    //  Least recently used queue of available workers
    int capacity = 0;
    zlist_t *workers = zlist_new ();

    zpoller_t *backends = zpoller_new (localbe, cloudbe, NULL);
    zpoller_t *frontends = zpoller_new (localfe, cloudfe, NULL);
    while (true) {
        //  First, route any waiting replies from workers
        //  If we have no workers, wait indefinitely
        zsock_t *ready = zpoller_wait (backends, capacity? 1000 * ZMQ_POLL_MSEC: -1);
        if (zpoller_terminated (backends))
            break;              //  Interrupted

        //  Handle reply from local worker
        zmsg_t *msg = NULL;
        if (ready == localbe) {
            msg = zmsg_recv (localbe);
            if (!msg)
                break;          //  Interrupted
            zframe_t *identity = zmsg_unwrap (msg);
            zlist_append (workers, identity);
            capacity++;

            //  If it's READY, don't route the message any further
            zframe_t *frame = zmsg_first (msg);
            if (memcmp (zframe_data (frame), WORKER_READY, 1) == 0)
                zmsg_destroy (&msg);
        }
        //  Or handle reply from peer broker
        else
        if (ready == cloudbe) {
            msg = zmsg_recv (cloudbe);
            if (!msg)
                break;          //  Interrupted
            //  We don't use peer broker identity for anything
            zframe_t *identity = zmsg_unwrap (msg);
            zframe_destroy (&identity);
        }
        //  Route reply to cloud if it's addressed to a broker
        for (argn = 2; msg && argn < argc; argn++) {
            char *data = (char *) zframe_data (zmsg_first (msg));
            size_t size = zframe_size (zmsg_first (msg));
            if (size == strlen (argv [argn])
            &&  memcmp (data, argv [argn], size) == 0)
                zmsg_send (&msg, cloudfe);
        }
        //  Route reply to client if we still need to
        if (msg)
            zmsg_send (&msg, localfe);

        //  .split route client requests
        //  Now we route as many client requests as we have worker capacity
        //  for. We may reroute requests from our local frontend, but not from 
        //  the cloud frontend. We reroute randomly now, just to test things
        //  out. In the next version, we'll do this properly by calculating
        //  cloud capacity:

        while (capacity) {
            zsock_t *ready = zpoller_wait (frontends, 0);
            int reroutable = 0;
            //  We'll do peer brokers first, to prevent starvation
            if (ready == cloudfe) {
                msg = zmsg_recv (cloudfe);
                reroutable = 0;
            }
            else
            if (ready == localfe) {
                msg = zmsg_recv (localfe);
                reroutable = 1;
            }
            else
                break;      //  No work, go back to backends
            //  If reroutable, send to cloud 20% of the time
            //  Here we'd normally use cloud status information
            //
            if (reroutable && argc > 2 && randof (5) == 0) {
                //  Route to random broker peer
                int peer = randof (argc - 2) + 2;
                zmsg_pushmem (msg, argv [peer], strlen (argv [peer]));
                zmsg_send (&msg, cloudbe);
            }
            else {
                zframe_t *frame = (zframe_t *) zlist_pop (workers);
                zmsg_wrap (msg, frame);
                zmsg_send (&msg, localbe);
                capacity--;
            }
        }
    }
    //  When we're done, clean up properly
    while (zlist_size (workers)) {
        zframe_t *frame = (zframe_t *) zlist_pop (workers);
        zframe_destroy (&frame);
    }
    zlist_destroy (&workers);

    for (actor_nbr = 0; actor_nbr < NBR_WORKERS + NBR_CLIENTS; actor_nbr++)
        zactor_destroy (&actors[actor_nbr]);

    zpoller_destroy (&frontends);
    zpoller_destroy (&backends);
    zsock_destroy (&cloudfe);
    zsock_destroy (&cloudbe);
    zsock_destroy (&localfe);
    zsock_destroy (&localbe);
    return EXIT_SUCCESS;
}
