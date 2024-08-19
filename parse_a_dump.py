#!/usr/bin/env python3

import re

def process_file(filename):
    pattern = re.compile(r'D \(\d+\) (\w+): (0x[0-9a-f]+)\s+((?:[0-9a-f]{2}\s+){16})\|')
    all_raw_data = bytes()

    with open(filename, 'r') as file:
        for line in file:
            match = pattern.match(line)
            if match:
                raw_data = bytes.fromhex(match.group(3).replace(' ', ''))
                all_raw_data += raw_data

    return all_raw_data

def find_repeating_patterns(data, min_length=4, max_length=1024):
    patterns = {}
    data_length = min(len(data), max_length)

    for i in range(int(data_length/2)):
        print(f'Searching patterns of length {i} / {data_length}')
        for j in range(i + min_length, data_length + 1):
            pattern = data[i:j]
            if pattern in patterns:
                patterns[pattern] += 1
            else:
                patterns[pattern] = 1

    repeating_patterns = {k: v for k, v in patterns.items() if v > 1}
    return repeating_patterns

# Example usage
filename = "a_dump.txt"
raw_data = process_file(filename)
#repeating_patterns = find_repeating_patterns(raw_data)

#print("Repeating patterns found:")
#for pattern, count in repeating_patterns.items():
#    print(f"Pattern: {pattern}, Count: {count}")

with open('raw_dump.mp3', 'wb') as f:
  f.write(raw_data)

