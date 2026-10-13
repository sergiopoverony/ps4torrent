#!/bin/bash
cd /tmp/ft; timeout 150 python3 mix_test.py daemon_host > mix.result 2>&1; cat mix.result
