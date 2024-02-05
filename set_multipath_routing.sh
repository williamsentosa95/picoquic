## Script to set up multipath routing on Linux as described here:
## https://multipath-tcp.org/pmwiki.php/Users/ConfigureRouting

#!/bin/bash

# Define your variables
ip_address_1="192.168.64.17"
ip_address_2="192.168.64.18"
subnet_cidr_1="192.168.64.0/24"
subnet_cidr_2="192.168.64.0/24"
interface1="enp0s1"
interface2="enp0s2"
gateway1="0.0.0.0"
gateway2="0.0.0.0"

# Apply the rules and routes using the defined variables
sudo ip rule add from $ip_address_1 table 1
sudo ip rule add from $ip_address_2 table 2

sudo ip route add $subnet_cidr_1 dev $interface1 scope link table 1
sudo ip route add default via $gateway1 dev $interface1 table 1

sudo ip route add $subnet_cidr_2 dev $interface2 scope link table 2
sudo ip route add default via $gateway2 dev $interface2 table 2

sudo ip route add default scope global nexthop via $gateway1 dev $interface1
