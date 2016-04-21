#!/usr/bin/env python3

from datetime import datetime
from random import shuffle
import sys
import json
import math
import glob
import statistics
from functools import wraps

from flask import Flask, request, abort, make_response, Response, session

def requires_auth(f):
    @wraps(f)
    def decorated(*args, **kwargs):
        auth = request.authorization
        if not auth or auth.password != WORKER_TOKEN:
            return Response(
                'Could not verify your access level for that URL.\n'
                'You have to login with proper credentials', 401,
                {'WWW-Authenticate': 'Basic realm="Login Required"'})
        return f(*args, **kwargs)
    return decorated

if len(sys.argv) != 2:
    print('Usage:')
    print('cluster/server.py [token]')

WORKER_TOKEN = sys.argv[1]

status = 'stopped'
workers = []
solver = None
total_assemblies = None
assemblies_unsolved = None
next_unsolved_index = 0
total_run = 0
solutions = []

def set_solver(text):
    global solver
    global total_assemblies
    global assemblies_unsolved
    global next_unsolved_index
    global total_run
    global solutions

    solver = text

    lines = text.split('\n')

    patterns = []
    depth = 1

    for line in lines:
        line = line.strip()

        if line.startswith('pattern '):
            patterns.append(line[8:].strip())
            continue

        if line.startswith('depth '):
            depth = int(line[6:].strip())

    total_assemblies = len(patterns) ** depth
    assemblies_unsolved = [a for a in range(total_assemblies)]
    next_unsolved_index = 0
    total_run = 0
    solutions = []

with open('solvers/gravity.solve') as f:
    set_solver(f.read())

app = Flask(__name__)

@app.route('/')
@requires_auth
def index():
    solvers = []

    for solver_fn in glob.glob('solvers/*.solve'):
        with open(solver_fn) as f:
            name = solver_fn.replace('solvers/', '').replace('.solve', '')
            text = f.read().replace('\n', '\\n')
            solvers.append("{name: '"+name+"',text: '"+text+"'}")

    with open('cluster/index.html') as f:
        ret = f.read()

        return ret.replace('{solvers}', ','.join(solvers))

@app.route('/view.js')
@requires_auth
def view():
    with open('cluster/view.js') as f:
        return f.read()

@app.route('/style.css')
@requires_auth
def style():
    with open('cluster/style.css') as f:
        res = make_response(f.read(), 200)

    res.headers['Content-Type'] = 'text/css'

    return res

@app.route('/console_update', methods=['POST'])
@requires_auth
def console_update():
    global status
    global workers
    global solutions
    global assemblies_unsolved

    req = request.get_json()

    if 'command' in req:
        if req['command'] == 'stop':
            status = 'stopped'
            solver = None

        elif req['command'] == 'run':
            status = 'running'

        elif req['command'] == 'pause':
            status = 'paused'

        elif req['command'] == 'unpause':
            status = 'stopped'

        elif req['command'] == 'reset':
            if status == 'running':
                status = 'stopped'

            workers = []
            set_solver(req['solver'])

    res = {
        'status': status,
        'programs_run': total_run,
        'workers': [],
        'solutions': solutions,
        'unsolved': assemblies_unsolved,
        'solved': [],
    }

    for w in workers:
        worker_status = 'inactive'

        if w['last_status_sent'] == 'paused':
            worker_status = 'paused'
        elif (datetime.now() - w['last_checkin']).total_seconds() < 5:
            worker_status = 'active'

        res['workers'].append({
            'worker_id': w['worker_id'],
            'cores': w['cores'],
            'run_rate': w['run_rate'],
            'assemblies_completed': len(w['assemblies_completed']),
            'programs_run': w['programs_run'],
            'status': worker_status,
        })

        res['solved'].extend([{
            'assembly': a['assembly'],
            'programs_completed': a['programs_completed'],
        } for a in w['assemblies_completed']])

    return json.dumps(res)

@app.route('/worker/status', methods=['POST'])
def worker_status():
    global assemblies_unsolved
    global next_unsolved_index
    global total_run
    global status
    global solutions

    req = request.get_json()

    if req['token'] != WORKER_TOKEN:
        abort(403)

    worker = next((w for w in workers if w['worker_id'] == req['worker_id']),
            None)

    if worker is None:
        worker = {
            'worker_id': req['worker_id'],
            'cores': req['cores'],
            'last_solver_sent': None,
            'assemblies_completed': [],
            'run_samples': [],
            'run_rate': None,
        }

        workers.append(worker)

    worker['last_checkin'] = datetime.now()
    worker['assemblies_running'] = req['assemblies_running']
    worker['assemblies_queued'] = req['assemblies_queued']

    if status == 'running':
        # Remove assemblies from the unsolved list if they've been completed
        if 'assemblies_completed' in req:
            for ac_new in req['assemblies_completed']:
                found = False

                for ac_old in worker['assemblies_completed']:
                    if ac_old['assembly'] == ac_new['assembly']:
                        found = True
                        break

                if not found:
                    worker['assemblies_completed'].append(ac_new)
                    total_run += ac_new['programs_completed']
                    solutions.extend(ac_new['solutions'])

            ac = [a['assembly'] for a in req['assemblies_completed']]
            assemblies_unsolved = [a for a in assemblies_unsolved
                                   if a not in ac]

    # End execution
    if assemblies_unsolved is None or len(assemblies_unsolved) == 0:
        status = 'stopped'

    # Calculate run sample
    worker['programs_run'] = sum(a['programs_completed']
        for a in worker['assemblies_completed'] + worker['assemblies_running'])

    worker['run_samples'].append({
        'sample': worker['programs_run'],
        'timestamp': datetime.now(),
    })

    if len(worker['run_samples']) > 3:
        del worker['run_samples'][0]

    # Calculate run rate
    if len(worker['run_samples']) <= 1:
        worker['run_rate'] = None
    else:
        run_rates = []

        for i in reversed(range(len(worker['run_samples']) - 1)):
            earlier = worker['run_samples'][i]
            later = worker['run_samples'][i + 1]

            run_rates.append(
                later['sample'] - earlier['sample'] /
                (later['timestamp'] - earlier['timestamp']).total_seconds())

        worker['run_rate'] = math.ceil(statistics.mean(run_rates))

    # Respond to worker
    ret = {
        'status': status,
    }

    worker['last_status_sent'] = status

    # Send the solver if needed
    if 'first_status' in req and req['first_status'] or \
       worker['last_solver_sent'] is None or \
       worker['last_solver_sent'] != solver:
        ret['solver'] = solver
        worker['last_solver_sent'] = solver

    if ret['status'] == 'running':
        # Workers should have twice their number of cores in assemblies
        # running/queued at all times, unless there aren't enough unsolved
        # assemblies left
        current = len(worker['assemblies_running']) + \
                len(worker['assemblies_queued'])

        ideal = worker['cores'] * 2

        needed = min(ideal - current, len(assemblies_unsolved))

        if needed > 0:
            assigned = []

            while len(assigned) < needed:
                if next_unsolved_index > len(assemblies_unsolved) - 1:
                    next_unsolved_index = 0

                assigned.append(assemblies_unsolved[next_unsolved_index])

                next_unsolved_index += 1

            ret['next_assemblies'] = assigned

    return json.dumps(ret)

if __name__ == '__main__':
    app.run(debug=True)
