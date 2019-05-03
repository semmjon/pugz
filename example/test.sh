#!/bin/bash
if  [ ! -f docword.nytimes.txt.gz ]; then
    wget https://s3-eu-west-1.amazonaws.com/artm/docword.nytimes.txt.gz
fi
echo -e "\n\ndecompressing with system's gunzip"
time gunzip -c docword.nytimes.txt.gz  >/dev/null
echo -e "\ndecompressing with pugz, 1 thread"
time ../gunzip docword.nytimes.txt.gz  >/dev/null
echo -e "\ndecompressing with pugz, 8 threads"
time ../gunzip -t 8 docword.nytimes.txt.gz  >/dev/null

echo -e "\n\ncounting lines with system's gunzip and wc -l"
time gunzip -c docword.nytimes.txt.gz | wc -l
echo -e "\ncounting lines with pugz, 1 thread"
time ../gunzip -l docword.nytimes.txt.gz
echo -e "\ncounting lineswith pugz, 8 threads"
time ../gunzip -l -t 8 docword.nytimes.txt.gz
