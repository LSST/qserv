# Standard
from itertools import ifilter
import time

# Package imports
import app

# Main AppInterface class
#
# We expect front-facing "normal usage" to come through this
# interface, which is intended to make a friendly wrapper around the
# functionality from the app module. 
class AppInterface:
    def __init__(self, reactor=None):
        self.tracker = app.TaskTracker()
        okname = ifilter(lambda x: "_" not in x, dir(self))
        self.publishable = filter(lambda x: hasattr(getattr(self,x), 
                                                    'func_doc'), 
                                  okname)
        self.reactor = reactor
        self.pmap = app.makePmap()
        pass

    def query(self, q, hints):
        "Issue a query. q=querystring, h=hint list"
        a = app.HintedQueryAction(q, hints, self.pmap)
        # FIXME: Need to fix task tracker.
        #taskId = self.tracker.track("myquery", a, q)
        #stats = time.qServQueryTimer[time.qServRunningName]
        #stats["appInvokeStart"] = time.time()
        if 'lsstRunning' in dir(self.reactor):
            self.reactor.callInThread(a.invoke3)
        else:
            a.invoke()
            #stats["appInvokeFinish"] = time.time()
        return 1 ## FIXME
        return taskId

    def help(self):
        """A brief help message showing available commands"""
        r = "" ## self._handyHeader()
        r += "\n<pre>available commands:\n"
        sorted =  map(lambda x: (x, getattr(self, x)), self.publishable)
        sorted.sort()
        for (k,v) in sorted:
            r += "%-20s : %s\n" %(k, v.func_doc)
        r += "</pre>\n"
        return r


    def check(self, taskId):
        "Check status of query or task. Params: "
        a = app.CheckAction(self.tracker, taskId)
        a.invoke()
        return a.results

    def results(self, taskId):
        "Get results location for a query or task. Params: taskId"
        return app.results(self.tracker, taskId)


    def reset(self):
        "Resets/restarts server uncleanly. No params."
        if self.reactor:
            args = sys.argv #take the original arguments
            args.insert(0, sys.executable) # add python
            os.execv(sys.executable, args) # replace self with new python.
            print "Reset failed:",sys.executable, str(args)
            return # This will not return.  os.execv should overwrite us.
        else:
            print "<Not resetting: no reactor>"

    def stop(self):
        "Unceremoniously stop the server."
        if self.reactor:
            self.reactor.stop()
    pass
