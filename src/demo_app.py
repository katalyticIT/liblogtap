#!/usr/bin/python3

##
## demo application
##
## * writes every second a line of text
## * into a fixed path logfile
## * which gets intercepted by liblogtap.so
##
## This file is part of github.com/katalyticIT/liblogtap
##

#-- import modules
import random
import time
import datetime

#-- definitions
naptime  = 2
logfile  = "/tmp/demo.log"
loremtxt = "Lorem ipsum dolor sit amet, consetetur sadipscing elitr, sed diam nonumy eirmod tempor invidunt ut labore et dolore magna aliquyam erat, sed diam voluptua. At vero eos et accusam et justo duo dolores et ea rebum. Stet clita kasd gubergren, no sea takimata sanctus est Lorem ipsum dolor sit amet. Lorem ipsum dolor sit amet, consetetur sadipscing elitr, sed diam nonumy eirmod tempor invidunt ut labore et dolore magna aliquyam erat, sed diam voluptua. At vero eos et accusam et justo duo dolores et ea rebum. Stet clita kasd gubergren, no sea takimata sanctus est Lorem ipsum dolor sit amet."

#-- open logfile and loop writing & sleeping
with open(logfile, 'w') as f:

  #-- loop writing & sleeping
  while True:
    txt_start = random.randint(1, 70)					# start somewhere in lorem text
    content   = loremtxt[txt_start:txt_start+20]			# copy 20 chars from there
    datestamp = datetime.datetime.now().strftime("%Y-%m-%d-%H.%M.%S")	# add date & time

    f.write( datestamp + " " + content + "\n" )	# put it all together
    time.sleep(naptime)				# have a little nap

