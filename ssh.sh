#!/bin/bash

PORT=${1:-9000}

ssh -p $PORT nk@localhost
