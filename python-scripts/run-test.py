import subprocess
import sys
import time
import pexpect

# Constant
HOME_FOLDER = "/home/william/"
BASE_FOLDER =  HOME_FOLDER + "picoquic-project/picoquic/"

# NOTES: If you want to generate new key, use this command
# openssl req -nodes -x509 -newkey rsa:2048 -days 365 -keyout ca-key.pem -out ca-cert.pem
# openssl req -nodes -newkey rsa:2048 -keyout server-key.pem -out server-req.pem

### CA Keys
CA_KEY_FOLDER = BASE_FOLDER + "web_browsing/"
SERVER_KEY = CA_KEY_FOLDER + "server-key.pem"
SERVER_CERT = CA_KEY_FOLDER + "ca-cert.pem"

SERVER_WEB_FOLDER = HOME_FOLDER + "webtest"
PASSWORD = "1234"

### Network emulation config
SERVER_PORT = 9000

NETWORK_TRACE_FOLDER = HOME_FOLDER + "/adv-mahimahi/multipath-network-emulator/traces/"
WEB_BROWSING_BASE_FOLDER = BASE_FOLDER + "web_browsing/dep_graphs/"

def run_picoquic_server(mp_option):
    if (mp_option < 0):
        mp_option = 0

    server_program = BASE_FOLDER + "picoquic_server"
    cmd = [server_program, "-k", SERVER_KEY, "-c", SERVER_CERT, "-p", str(SERVER_PORT), "-w", SERVER_WEB_FOLDER, "-E", str(mp_option)]
    process = subprocess.Popen(cmd)
    return process

def get_mm_multipath_cmd():
    # Queue configs
    uplink_queue_1_policy = "--uplink-queue-1=" + "droptail"
    uplink_queue_1_args = "--uplink-queue-args-1=packets=" + "10000"
    downlink_queue_1_policy = "--downlink-queue-1=" + "droptail"
    downlink_queue_1_args = "--downlink-queue-args-1=packets=" + "10000"

    uplink_queue_2_policy = "--uplink-queue-2=" + "droptail"
    uplink_queue_2_args = "--uplink-queue-args-2=packets=" + "10000"
    downlink_queue_2_policy = "--downlink-queue-2=" + "droptail"
    downlink_queue_2_args = "--downlink-queue-args-2=packets=" + "10000"

    path_1_latency_trace = NETWORK_TRACE_FOLDER + "delay-traces/" + "latency-60ms-rtt"
    path_1_bw_trace = NETWORK_TRACE_FOLDER + "bandwidth-traces/" + "180Mbps"
    path_1_packet_log_folder = HOME_FOLDER + "/test-1" # Make sure that this folder exist

    path_2_latency_trace = NETWORK_TRACE_FOLDER + "delay-traces/" + "latency-4ms-rtt"
    path_2_bw_trace = NETWORK_TRACE_FOLDER + "bandwidth-traces/" + "2Mbps"
    path_2_packet_log_folder = HOME_FOLDER + "/test-2" # Make sure that this folder exist

    total_num_args = 1

    cmd = [
        "mm-multipath",
        str(total_num_args),
        uplink_queue_1_policy, uplink_queue_1_args,
        downlink_queue_1_policy, downlink_queue_1_args,
        uplink_queue_2_policy, uplink_queue_2_args,
        downlink_queue_2_policy, downlink_queue_2_args,
        path_1_latency_trace, path_1_bw_trace, path_1_bw_trace, path_1_packet_log_folder,
        path_2_latency_trace, path_2_bw_trace, path_2_bw_trace, path_2_packet_log_folder
    ]

    # Update the total num_args
    cmd[1] = str(len(cmd))

    return cmd

def run_client(mp_option):
    cmd = []
    mm_multipath_cmd = get_mm_multipath_cmd()
    
    http3_client_mp_program = BASE_FOLDER + "/http3_client_mp"
    http3_client_sp_program = BASE_FOLDER + "/http3_client"
    traffic_trace_file = BASE_FOLDER + "web_browsing/traces/" + "test.csv"
    net_logfpath = HOME_FOLDER + "/picoquic-log/microsoft-net.csv"
    client_program_cmd = []
    
    if (mp_option >= 0):
        client_program_cmd = [http3_client_mp_program, str(mp_option), traffic_trace_file]
    else:
        client_program_cmd = [http3_client_sp_program, traffic_trace_file]

    cmd = mm_multipath_cmd + client_program_cmd

    process = subprocess.Popen(cmd)

    return process

def main(args):
    mp_option = int(args[0])

    server_process = run_picoquic_server(mp_option)
    client_process = run_client(mp_option)
    
    client_process.wait()
    server_process.terminate()
    # time.sleep(5)
    return 0

if __name__ == '__main__':
	prog = sys.argv[0]
	args = sys.argv[1:]
	num_args = len(args)

	if (num_args < 1) :
		sys.stderr.write((u"Usage: %s" +
						  u" <mp-option>\n") %
						 (prog))
		sys.exit(1)

	main(args)