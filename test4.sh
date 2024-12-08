#!/bin/bash
make

# Create the filesystem
./create_fs disk1
./create_fs disk2

# Run the program and redirect stdout and stderr
./fs input > output.log 2> error.log

# Function to compare files and display differences, ignoring \r (carriage return) characters
compare_files() {
    local file1=$1
    local file2=$2
    local output_type=$3

    echo "Comparing $output_type..."

    # Remove carriage return characters (\r) from both files before comparing
    diff -u --color=always <(sed 's/\r//g' "$file1") <(sed 's/\r//g' "$file2")

    # Check if the files match
    if [ $? -eq 0 ]; then
        echo "$output_type matches expected output!"
    else
        echo "$output_type does not match expected output! Differences:"
        diff -u --color=always <(sed 's/\r//g' "$file1") <(sed 's/\r//g' "$file2")
    fi
    echo
}

# Compare stdout
compare_files output.log ./tests/test3/stdout_expected "stdout"

# Compare stderr
compare_files error.log ./tests/test3/stderr_expected "stderr"

# Compare disk with expected disk, ignoring carriage return characters
echo "Comparing disk with expected disk..."
cmp -b <(sed 's/\r//g' disk1) <(sed 's/\r//g' ./tests/test3/disk1_expected)

if [ $? -eq 0 ]; then
    echo "Disk matches expected output!"
else
    echo "Disk does not match expected output!"
fi

echo "Comparing disk with expected disk..."
cmp -b <(sed 's/\r//g' disk2) <(sed 's/\r//g' ./tests/test3/disk2_expected)

if [ $? -eq 0 ]; then
    echo "Disk matches expected output!"
else
    echo "Disk does not match expected output!"
fi
