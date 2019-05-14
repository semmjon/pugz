#!/bin/bash

# same as test.sh but with bigger files
# script used to make the results in the README
# if you're in a hurry and just want to test pugz, use test.sh      

timee () {
    start=$(date +%s.%N)
    time sh -c "$1"
    end=$(date +%s.%N)
    runtime=$(echo "$end - $start" | bc)
    echo "runtime $runtime"
    mbs=$(echo "$2.0 / 1024.0 / 1024.0 / $runtime" | bc)
    echo "speed $mbs MB/s"
}

benchmark () {
    file=$1
    basefile=$(basename $file)
    
    if  [ ! -f $basefile ]; then
        wget $file
    fi
    echo $basefile
    filesize=$(stat -c%s "$basefile")
    
    echo -e "\n\ndecompressing with system's gunzip"
    timee "gunzip -c $basefile  >/dev/null" $filesize

    echo -e "\ndecompressing with pugz, 1 thread"
    timee "../gunzip $basefile  >/dev/null" $filesize

    echo -e "\ndecompressing with pugz, 8 threads"
    timee "../gunzip -t 8 $basefile  >/dev/null" $filesize

    echo -e "\n\ncounting lines with system's gunzip and wc -l"
    timee "gunzip -c $basefile | wc -l" $filesize
    
    echo -e "\ncounting lines with pugz, 1 thread"
    timee "../gunzip -l $basefile" $filesize
    
    echo -e "\ncounting lineswith pugz, 8 threads"
    timee "../gunzip -l -t 8 $basefile" $filesize
}   
    
benchmark ftp://ftpmirror.your.org/pub/wikimedia/dumps/20110115/enwiki-20110115-externallinks.sql.gz    
benchmark ftp://ftpmirror.your.org/pub/wikimedia/dumps/20110115/enwiki-20110115-pagelinks.sql.gz  
