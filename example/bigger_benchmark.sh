#!/bin/bash

# same as test.sh but with bigger files
# script used to make the results in the README
# if you're in a hurry and just want to test pugz, use test.sh      

timee () {
    cmd_line=$1
    filename=$2
    filesize=$(stat -Lc%s "${filename}")

    # Preload file into system page cache
    cat "${filename}" > /dev/null
    # Measure wall clock time
    start=$(date +%s.%N)
    time sh -c "$cmd_line"
    end=$(date +%s.%N)

    runtime=$(echo "$end - $start" | bc)
    echo "runtime $runtime"
    mbs=$(echo "${filesize}.0 / 1024.0 / 1024.0 / $runtime" | bc)
    echo "speed $mbs MB/s"
}

# Get the number of CPU threads and generate a sequence of numbers of threads
i=$(nproc)
nthreads=($i)
while [ $i -gt 1 ]
do
    i=$[$i/2]
    nthreads=($i "${nthreads[@]}")
done

benchmark () {
    url=$1
    basefile=$(basename $url)
    if [[ ! -f "$basefile" ]]; then
        if [[ "$url" == *"://"* ]]; then
            wget $url
        else
            return
        fi
    fi

    echo $basefile

    echo -e "\n\ndecompressing with system's gunzip"
    timee "gunzip -c $basefile  >/dev/null" $basefile

    for i in "${nthreads[@]}"
    do
        echo -e "\ndecompressing with pugz, $i threads"
        timee "../gunzip $basefile -t $i >/dev/null" $basefile
    done

    echo -e "\n\ncounting lines with system's gunzip and wc -l"
    timee "gunzip -c $basefile | wc -l" $basefile
    
    for i in "${nthreads[@]}"
    do
        echo -e "\ncounting lines with pugz, $i threads"
        timee "../gunzip -l -t $i $basefile" $basefile
    done

}   
    
benchmark http://archive.ics.uci.edu/ml/machine-learning-databases/00280/HIGGS.csv.gz
benchmark SRR6832872.fastq.gz # You need to fastq-dump this one (24G), otherwise it is simply ignored
