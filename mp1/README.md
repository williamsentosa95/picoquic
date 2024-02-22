# Running the TCP client and server

- Run ```make``` in the ```mp1``` directory to create the executables
- Run 

```
./http_server
```

to start the server

- Run 

```
./http_client localhost <n_iterations> <output_csv_file> <message_size> <sleep_time>
```

to run the client where

    - **n_iterations**: the number of messages to be sent
    - **output_csv_file**: file where the RTT values in microseconds will be stored. Number of 
        values stored will equal ```n_iterations```
    - **message_size**: number of bytes in the message. Should not exceed 2048575 as the server
        stores only that much data in memory
    - **sleep_time**: time the client sleeps between successive messages

- Run 

```
python3 mean_std.py
```

to compute mean and standard deviation of the data in milliseconds.

The csv file name should be changed appropriately within the python script.