#!/bin/bash -ev

mkdir -p ~/.Brycecoin
echo "rpcuser=username" >>~/.Brycecoin/Brycecoin.conf
echo "rpcpassword=`head -c 32 /dev/urandom | base64`" >>~/.Brycecoin/Brycecoin.conf

