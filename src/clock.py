
#
# ** clock.py **
#
# Simple app to produce log output inside the
# liblogtap demo deployment; just prints the
# time in an endless loop (default: every 2s).
#

#-- config
sleeptime = 2

#-- libraries
from datetime import datetime
import time

#-- main()
while True:
  current_time = datetime.now().strftime("%H:%M:%S")
  print(current_time)
  time.sleep(sleeptime)


