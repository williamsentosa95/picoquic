import numpy as np 
import pandas as pd 

# server_file = "../latency_dl/TCPserver_1GB.csv"
# client_file = "../latency_dl/TCPclient_1GB.csv"

server_file = "../latency_dl/TCPserver_1GB_100Mbps.csv"
client_file = "../latency_dl/TCPclient_1GB_100Mbps.csv"

# server_file = "../latency_dl/TCPserver_1MB.csv"
# client_file = "../latency_dl/TCPclient_1MB.csv"

# server_file = "../latency_dl/TCPserver_100KB.csv"
# client_file = "../latency_dl/TCPclient_100KB.csv"
 
# server_file = "../latency_dl/TCPserver_10KB.csv"
# client_file = "../latency_dl/TCPclient_10KB.csv"

server_df = pd.read_csv(server_file, header=None)
client_df = pd.read_csv(client_file, header=None)


latency = client_df[0] - server_df[0]
print(latency)
print(latency.mean()/1000)


# server = [1705049147364560,
# 1705049147364609,
# 1705049147364618,
# 1705049147364637,
# 1705049147364666,
# 1705049147364722,
# 1705049147364731,
# 1705049147364742,
# 1705049147364787,
# 1705049147364796,
# 1705049147364806,
# 1705049147364814,
# 1705049147364835,
# 1705049147364845,
# 1705049147364852,
# 1705049147364860,
# 1705049147364869,
# 1705049147364889,
# 1705049147364897,
# 1705049147364904,
# 1705049147364914,
# 1705049147364922,
# 1705049147364929,
# 1705049147364985,
# 1705049147364995]

# client = [
#     1705049147365441,
# 1705049147365486,
# 1705049147365656,
# 1705049147365709,
# 1705049147365747,
# 1705049147365788,
# 1705049147365860,
# 1705049147365889,
# 1705049147365926,
# 1705049147365972,
# 1705049147365995,
# 1705049147366010,
# 1705049147366031,
# 1705049147366045,
# 1705049147366095,
# 1705049147366150,
# 1705049147366198,
# 1705049147366216,
# 1705049147366249,
# 1705049147366291,
# 1705049147366314,
# 1705049147366329,
# 1705049147366350,
# 1705049147366365,
# 1705049147366385
# ]

# latency = np.array(client) - np.array(server)
# print(latency)