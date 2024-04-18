import subprocess
import sys
import fileinput
import time


class Experiment_Runner:
    def __init__(self, base_folder, cert_password):
        self.base_folder = base_folder
        self.cert_password = cert_password

    def run_experiments_with_5G_traces(self, schemes, scenarios, tracefile, timeout, results_folder, wait_period):
        print("Running experiments with 5G traces")
        subprocess.run("killall mm-multipath",
                       shell=True, stderr=subprocess.PIPE)
        time.sleep(2)

        for scheme in schemes:
            for scenario in scenarios:
                print(f"Scheme: {scheme} Scenario: {scenario}")

                mm_cmd = f"mm-multipath 10 ~/5G-traces/{scenario}/latency ~/5G-traces/{scenario}/upload-bandwidth ~/5G-traces/{scenario}/download-bandwidth $HOME/logs/iface1 $MAHIMAHI_ROOT/traces/delay-traces/latency-5ms-rtt $MAHIMAHI_ROOT/traces/bandwidth-traces/2Mbps $MAHIMAHI_ROOT/traces/bandwidth-traces/2Mbps $HOME/logs/iface2"
                mm_proc = subprocess.Popen(
                    f"{mm_cmd}", shell=True, cwd=f"{self.base_folder}", stdin=subprocess.PIPE, stdout=subprocess.PIPE)
                mm_proc.stdin.write(
                    f"./receiver_{scheme} {timeout} {wait_period} > {results_folder}/receiver_{scheme}_{scenario}.log\n".encode())
                mm_proc.stdin.flush()
                time.sleep(2)

                subprocess.run(
                    f"./sender_{scheme} {tracefile} {timeout} > {results_folder}/sender_{scheme}_{scenario}.log ", shell=True, cwd=f"{self.base_folder}")

                time.sleep(20)
                subprocess.run(f"killall receiver_{scheme}", shell=True)
                subprocess.run("killall mm-multipath", shell=True)
                time.sleep(2)


if __name__ == "__main__":
    base_folder = "/home/alienware/mpquic_project/picoquic_svc/SVC"
    cert_password = "1234\n"
    exp_runner = Experiment_Runner(base_folder, cert_password)

    # Experiments with 5G traces
    schemes = ["sp", "mp_original", "mp_priority"]
    # schemes = ["mp_original"]
    # scenarios = ["mmwave-driving", "lowband-driving", "mmwave-walking",
    #              "lowband-walking", "mmwave-stationary", "lowband-stationary"]
    scenarios = ["mmwave-walking"]
    tracefile = f"{base_folder}/12000_custom.trace"
    results_folder = f"{base_folder}/results"
    timeout = 60*5  # in seconds
    wait_period = 60  # in milliseconds

    exp_runner.run_experiments_with_5G_traces(
        schemes, scenarios, tracefile, timeout, results_folder, wait_period)
 