#include "udp.h"
#include <stdbool.h>
#include <stdlib.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "cassiemujoco.h"


enum mode {
    MODE_STANDARD,
    MODE_RL
};


static long diff_time_us(const struct timespec *a, const struct timespec *b)
{
    return (a->tv_sec - b->tv_sec) * 1000000 +
        (a->tv_nsec - b->tv_nsec) / 1000;
}


int main(int argc, char *argv[])
{
    // Option variables and flags
    char *iface_addr_str = "0.0.0.0";
    char *port_str = "25000";
    bool realtime = false;
    bool visualize = false;
    bool hold = false;
    char *log_file_path = NULL;
    char *qlog_file_path = NULL;
    int mode = MODE_STANDARD;

    // Parse arguments
    int c;
    while ((c = getopt(argc, argv, "a:p:rvhl:q:x")) != -1) {
        switch (c) {
        case 'a':
            // Inteface address to bind
            iface_addr_str = optarg;
            break;
        case 'p':
            // Port to bind
            port_str = optarg;
            break;
        case 'r':
            // Enable real-time mode
            realtime = true;
            break;
        case 'v':
            // Visualize simulation
            visualize = true;
            break;
        case 'h':
            // Hold pelvis
            hold = true;
            break;
        case 'l':
            // Log data
            log_file_path = optarg;
            break;
        case 'q':
            // Log simulator state data
            qlog_file_path = optarg;
            break;
        case 'x':
            // Run in RL mode
            mode = MODE_RL;
            break;
        default:
            // Print usage
            printf(
"Usage: cassiesim [OPTION]...\n"
"Simulates the Cassie robot communicating over UDP.\n"
"\n"
"  -a [ADDRESS]   Specify the local interface to bind to.\n"
"  -p [PORT]      Specify the port to listen on.\n"
"  -r             Run simulation continuously instead of in lockstep.\n"
"  -v             Show a visualization of the running simulation.\n"
"  -h             Hold the pelvis in place.\n"
"  -l [FILENAME]  Log the input and output UDP data to a file.\n"
"  -q [FILENAME]  Log simulation time, qpos, and qvel to a file.\n"
"  -x             Run in RL mode, taking PD targets and sending state estimates.\n"
"\n"
"By default, the simulator listens on all IPv4 interfaces at port %s.\n"
"If the option -r is not given, the simulator waits for input after sending\n"
"each sensor data packet. With -r, the simulator runs in real-time, reusing\n"
"old input if new data is not yet ready and waiting a minimum amount of time\n"
"between sending sensor data packets.\n\n", port_str);
            exit(EXIT_SUCCESS);
        }
    }

    // Bind to network interface
    int sock = udp_init(iface_addr_str, port_str, UDP_SERVER);
    if (-1 == sock)
        exit(EXIT_FAILURE);

    // Create packet input/output buffers
    int dinlen, doutlen;
    switch (mode) {
    case MODE_RL:
        dinlen = PD_IN_T_PACKED_LEN;
        doutlen = STATE_OUT_T_PACKED_LEN;
        break;
    default:
        dinlen = CASSIE_USER_IN_T_PACKED_LEN;
        doutlen = CASSIE_OUT_T_PACKED_LEN;
    }
    const int recvlen = PACKET_HEADER_LEN + dinlen;
    const int sendlen = PACKET_HEADER_LEN + doutlen;
    unsigned char *recvbuf = malloc(recvlen);
    unsigned char *sendbuf = malloc(sendlen);

    // Separate input/output buffers into header and payload
    const unsigned char *header_in = recvbuf;
    const unsigned char *data_in = &recvbuf[PACKET_HEADER_LEN];
    unsigned char *header_out = sendbuf;
    unsigned char *data_out = &sendbuf[PACKET_HEADER_LEN];

    // Create standard input/output structs
    cassie_user_in_t cassie_user_in = {0};
    cassie_out_t cassie_out;

    // Create RL input/output structs
    pd_in_t pd_in = {0};
    state_out_t state_out;

    // Create header information struct
    packet_header_info_t header_info = {0};

    // Address to send sensor data packets to
    struct sockaddr_storage src_addr = {0};
    socklen_t addrlen = sizeof src_addr;

    // Create cassie simulation
    cassie_sim_t *sim = cassie_sim_init();
    cassie_vis_t *vis;
    if (visualize)
        vis = cassie_vis_init();
    if (hold)
        cassie_sim_hold(sim);

    // Cassie input/output log file
    FILE *log_file = NULL;
    if (log_file_path)
        log_file = fopen(log_file_path, "w");

    // SImulator state log file
    FILE *qlog_file = NULL;
    if (qlog_file_path)
        qlog_file = fopen(qlog_file_path, "w");

    // Manage simulation loop
    unsigned long long loop_counter = 0;
    bool run_sim = false;
    struct timespec send_time, recv_time;
    clock_gettime(CLOCK_MONOTONIC, &send_time);
    clock_gettime(CLOCK_MONOTONIC, &recv_time);
    static const long cycle_usec = 1000000 / 2000;
    long timeout_usec;
    switch (mode) {
    case MODE_RL:
        timeout_usec = 100 * 1000;
        break;
    default:
        timeout_usec = 10 * 1000;
    }

    printf("Waiting for input...\n");

    // Listen/respond loop
    while (true) {
        // Try to get a new packet
        ssize_t nbytes;
        if (realtime) {
            // Get newest packet, or return -1 if no new packets are available
            nbytes = get_newest_packet(sock, recvbuf, recvlen,
                                       (struct sockaddr *) &src_addr, &addrlen);
        } else {
            // If not in real-time mode, wait until a new packet comes in
            nbytes = wait_for_packet(sock, recvbuf, recvlen,
                                     (struct sockaddr *) &src_addr, &addrlen);
        }

        // If a new packet was received, process and unpack it
        if (recvlen == nbytes) {
            // Process incoming header and write outgoing header
            process_packet_header(&header_info, header_in, header_out);
            printf("\033[F\033[Jdelay: %d, diff: %d\n",
                   header_info.delay, header_info.seq_num_in_diff);

            // Unpack received data into cassie user input struct
            switch (mode) {
            case MODE_RL:
                unpack_pd_in_t(data_in, &pd_in);
                break;
            default:
                unpack_cassie_user_in_t(data_in, &cassie_user_in);
            }

            // Update packet received timestamp
            clock_gettime(CLOCK_MONOTONIC, &recv_time);

            // Start the simulation after the first valid packet is received
            run_sim = true;
        }

        if (run_sim) {
            // Run simulator and pack output struct into outgoing packet
            switch (mode) {
            case MODE_RL:
                cassie_sim_step_pd(sim, &state_out, &pd_in);
                pack_state_out_t(&state_out, data_out);
                break;
            default:
                cassie_sim_step(sim, &cassie_out, &cassie_user_in);
                pack_cassie_out_t(&cassie_out, data_out);
            }

            // Log Cassie input/output
            if (log_file) {
                fwrite(data_out, doutlen, 1, log_file);
                fwrite(data_in, dinlen, 1, log_file);
            }

            // Log simulation state
            if (qlog_file) {
                fwrite(cassie_sim_time(sim), sizeof (double), 1, qlog_file);
                fwrite(cassie_sim_qpos(sim), sizeof (double), 35, qlog_file);
                fwrite(cassie_sim_qvel(sim), sizeof (double), 32, qlog_file);
            }

            if (realtime) {
                // Wait at least cycle_usec between sending simulator updates
                struct timespec current_time;
                do {
                    clock_gettime(CLOCK_MONOTONIC, &current_time);
                } while (diff_time_us(&current_time, &send_time) < cycle_usec);
                send_time = current_time;

                // Zero input data if no new packets have been
                // received in a while
                if (diff_time_us(&current_time, &recv_time) > timeout_usec) {
                    memset(&cassie_user_in, 0, sizeof cassie_user_in);
                    memset(&pd_in, 0, sizeof pd_in);
                }
            }

            // Send response, retry if busy
            while (-1 == sendto(sock, sendbuf, sendlen, 0,
                                (struct sockaddr *) &src_addr, addrlen)) {}
        }

        // Draw no more then once every 33 simulation steps
        if (visualize && loop_counter % 33 == 0)
            cassie_vis_draw(vis, sim);

        // Increment loop counter
        ++loop_counter;
    }
}