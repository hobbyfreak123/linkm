#!/bin/sh

# if you haven't made the jar:
#java thingm.linkm.LinkM $*

# but normally
if [ -e linkm.jar ]; then
    java -jar linkm.jar $*
else 
    echo "cannot run. make the jar with 'make jar' please"
fi
