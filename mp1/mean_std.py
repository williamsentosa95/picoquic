import csv
import numpy as np

# Replace 'your_file.csv' with the path to your CSV file
file_path = 'client_ts.csv'

# Initialize an empty list to store the data
data = []

# Read data from CSV file
with open(file_path, 'r') as csvfile:
    reader = csv.reader(csvfile)
    for row in reader:
        # Since there's only one column, take the first element and convert to float
        data.append(float(row[0]))

# Convert the list to a NumPy array for efficient numerical calculations
data_array = np.array(data)

# Compute mean
mean = np.mean(data_array)

# Compute standard deviation
std_dev = np.std(data_array)

# Print the results
print(f'Mean: {mean}')
print(f'Standard Deviation: {std_dev}')

