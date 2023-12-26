import re
from datetime import datetime, timedelta
import plotly.graph_objects as go
import plotly.express as px
import pandas as pd
import os
import json

def getTime(log, year):
    log_split = log.split()
    return str(year) + "-" + log_split[0] + " " + log_split[1]

#used for regex to find the data and time
time_pattern = "^[0-9]{2}-[0-9]{2}\s[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{3}"
extra_pattern = "\[EXTRA: .*\]"

path_to_script = os.path.dirname(os.path.abspath(__file__))
logs_path = os.path.join(path_to_script, "logs")
states_path = os.path.join(path_to_script, "states.json")

#events contains types of events (control, lease)
#an event contains states (disconnectd, confirmed)
events = list()
ticktext = list()
tickvals = list()

with open(os.path.join(states_path), encoding="utf-8") as file:
    data = json.load(file)
file.close()

for group_index, group in enumerate(data['groups']):
    events.append(group["group_name"])
    for state in group["states"]:
        ticktext.append(state["state"])
        tickvals.append(state["value"])

#data dict is used to store times, states, date, for ALL logcats
log_data = dict()
for event in events:
    log_data[event] = dict()
    log_data[event]["times"] = list()
    log_data[event]["states"] = list()
    log_data[event]["date"] = list()
    log_data[event]["labels"] = list()
    log_data[event]["extras"] = list()
    log_data[event]["annotations"] = list()

#contains number of wlan0 disconnects for each day
wlan_disconnect_data = dict()

#find all files in directory and subdirectory
for subdir, dirs, files in os.walk(logs_path):
    for file in files:
        filepath = subdir + os.sep + file

        #if it's a logfile txt
        if file.endswith(".txt") and "logcat" in file:
            with open(os.path.join(logs_path, filepath), encoding="utf-8", errors='ignore') as f:
                lines = [l for l in f]  #list of lines in the file
                
                year = file.split("-")[0]

                wlan0_count = 0

                #current data dict is used to store times, states, and date for CURRENT logcat - to be added to data dict later
                current_log_data = dict()
                for event in events:
                    current_log_data[event] = {
                        "times": list(),
                        "states": list(),
                        "date": list(),
                        "labels": list(),
                        "extras": list(),
                        "annotations": list(),
                    }

                lines.reverse()  #reverse so that if you detect a timeskip, you can update all the old times in one pass (instead of new times)
                previous_time = None
                difference = timedelta(0,0)
                timeskip = False
                year_changed = 0
                #iterate through the log file and add all confirmed event states and corresponding data to event lists
                for line_index, line in enumerate(lines):
                    
                    #if there's no time, skip - it's not a log
                    if(not (time_match := re.search(time_pattern, line))):
                        continue
                    
                    #check if there is a change in year
                    if(not year_changed and previous_time != None):
                        current_month = int(line.split("-")[0])
                        previous_month = int(previous_time.strftime('%Y-%m-%d %H:%M:%S.%f').split("-")[1])
                        #if going from 1 to 12, then subtract one year
                        if(current_month == 1 and previous_month == 12):
                            print("subtracted year")
                            year_changed = 1
                            year = int(year) + year_changed
                        #if 12 going to 1, then add one year
                        if(current_month == 12 and previous_month == 1):
                            print("added year")
                            year_changed = -1
                            year = int(year) + year_changed

                    #convert current time to datetime object
                    current_time = datetime.strptime(year + "-" + time_match.group(0), '%Y-%m-%d %H:%M:%S.%f')
                    
                    #if previous time != None and new time later than previous time, calculate difference
                    if(previous_time != None and (current_time - previous_time > timedelta(0,60) or current_time - previous_time < -timedelta(2, 0))):   #difference needs to be greater than 60 seconds
                        
                        #only calculates distance the first time there is a skip. ASSUMES ONLY ONE TIMESKIP PER FILE
                        if("setTimeZone" in line or "Setting time of day" in line):
                            #print("time skip detected: ", line, previous_time, difference)
                            difference = current_time - previous_time
                            timeskip = True

                        #add difference to current value to add to the dictionary
                        current_time = current_time - difference

                    #search line for conditionals
                    for group_index, group in enumerate(data['groups']):    #control, lease
                        event = events[group_index]
                        for state in group['states']: #disconnected, confirmed
                            for conditional in state["conditionals"]:
                                conditional_search = conditional["conditional"].replace("\\\\", "\\")
                                extra_search = conditional["extra"].replace("\\\\", "\\")

                                if conditional_search in line:
                                    #insert blip first
                                    if(conditional["blip"]):
                                        date_and_time = (current_time + timedelta(milliseconds=1)).strftime('%Y-%m-%d %H:%M:%S.%f')
                                        current_log_data[event]["times"].append(date_and_time)
                                        current_log_data[event]["states"].append(group['min_value'])
                                        current_log_data[event]["date"].append(date_and_time.split()[0])# + "_" + group["group_name"])   #only the date #(logname)
                                        current_log_data[event]["labels"].append("Blip down")  #remove data and time
                                        current_log_data[event]["extras"].append("")
                                        current_log_data[event]["annotations"].append("")

                                    date_and_time = current_time.strftime('%Y-%m-%d %H:%M:%S.%f')
                                    current_log_data[event]["times"].append(date_and_time)
                                    current_log_data[event]["states"].append(state["value"])
                                    current_log_data[event]["date"].append(date_and_time.split()[0])# + "_" + group["group_name"])   #only the date
                                    current_log_data[event]["labels"].append(' '.join(line.split()[2:]))  #remove data and time
                                    if(state == "CTRL-EVENT-DISCONNECTED" and "wlan0" in line):
                                        wlan0_count+=1

                                    if(extra_search != "" and extra_search in line):
                                        current_log_data[event]["extras"].append(extra_search)
                                        current_log_data[event]["annotations"].append(extra_search[0:1].upper())
                                    else:
                                        current_log_data[event]["extras"].append("")
                                        current_log_data[event]["annotations"].append("")
                        
                    #add wlan0 count to the previous count that day already had
                    wlan_disconnect_data[current_time.strftime('%Y-%m-%d %H:%M:%S.%f').split()[0]] = wlan_disconnect_data.setdefault(current_time.strftime('%Y-%m-%d %H:%M:%S.%f').split()[0], 0) + wlan0_count
                    wlan0_count = 0

                    #set previous time to current time
                    if('current_time' in locals()):
                        previous_time = current_time
                    else:
                        previous_time = datetime.strptime(year + "-" + time_match.group(0), '%Y-%m-%d %H:%M:%S.%f')

                #add points at the very beginning and end of graph to that plots have uniform start and end time
                for event_index, event in enumerate(events):
                    
                    #only do this if any relevant events were found in the logcat
                    if(len(current_log_data[event]["times"]) > 0 and not year_changed):
                        #sort the dates so that when you tack on the first and last dates, they go in the proper place
                        
                        current_log_data[event]["states"] = [v for _,v in sorted(zip(current_log_data[event]["times"],current_log_data[event]["states"]))]
                        current_log_data[event]["date"] = [w for _,w in sorted(zip(current_log_data[event]["times"],current_log_data[event]["date"]))]
                        current_log_data[event]["labels"] = [x for _,x in sorted(zip(current_log_data[event]["times"],current_log_data[event]["labels"]))]
                        current_log_data[event]["extras"] = [y for _,y in sorted(zip(current_log_data[event]["times"],current_log_data[event]["extras"]))]
                        current_log_data[event]["annotations"] = [z for _,z in sorted(zip(current_log_data[event]["times"],current_log_data[event]["annotations"]))]
                        current_log_data[event]["times"].sort()
                        
                        #prepend a value event lists to have the data on the graph stretch all the way
                        first_date_and_time = getTime(lines[1], year)
                        current_log_data[event]["times"].append(first_date_and_time)
                        #for the prepended state, have it be the deactivated event
                        #disconnected -> connected, disconnectd -> disconnectd both make sense
                        current_log_data[event]["states"].append(data['groups'][event_index]['min_value'])
                        current_log_data[event]["date"].append(first_date_and_time.split()[0])# + "_" + event)
                        current_log_data[event]["labels"].append("Assumed end state")
                        current_log_data[event]["extras"].append("")
                        current_log_data[event]["annotations"].append("")

                        #append a value to have data on graph stretch all the way. set its state equal to the last detected state
                        if(difference != 0):
                            last_date_and_time = (datetime.strptime(getTime(lines[-2], year), '%Y-%m-%d %H:%M:%S.%f') - difference).strftime('%Y-%m-%d %H:%M:%S.%f')
                        else:
                            last_date_and_time = getTime(lines[-2], year)
                        current_log_data[event]["times"].insert(0, last_date_and_time)
                        current_log_data[event]["states"].insert(0, current_log_data[event]["states"][-1])
                        current_log_data[event]["date"].insert(0, last_date_and_time.split()[0])# + "_" + event)
                        current_log_data[event]["labels"].insert(0, "Assumed initial state")
                        current_log_data[event]["extras"].insert(0, "")
                        current_log_data[event]["annotations"].insert(0, "")

                    #append current data
                    log_data[event]["times"].extend(current_log_data[event]["times"])
                    log_data[event]["states"].extend(current_log_data[event]["states"])
                    log_data[event]["date"].extend(current_log_data[event]["date"])
                    log_data[event]["labels"].extend(current_log_data[event]["labels"])
                    log_data[event]["extras"].extend(current_log_data[event]["extras"])
                    log_data[event]["annotations"].extend(current_log_data[event]["annotations"])
                    
                f.close()

#print(log_data)
#print(len(log_data[event]["times"]), len(log_data[event]["extras"]))

#turn data dict into a dictionary with dataframes holding all the event list values
data_dfs = dict()
for event in log_data:  #control, lease
    data_dfs[event] = pd.DataFrame({
        "times": log_data[event]["times"],
        "states": log_data[event]["states"],
        "date": log_data[event]["date"],
        "labels": log_data[event]["labels"],
        "extras": log_data[event]["extras"],
        "annotations": log_data[event]["annotations"],
    })
    data_dfs[event] = data_dfs[event].sort_values(by='times')

fig = go.Figure()

frames = list()
for event_type in events:
    if(not data_dfs[event_type].empty):
        frames.append(data_dfs[event_type])
        fig_to_add = px.line(data_dfs[event_type], x=data_dfs[event_type].times, y=data_dfs[event_type].states, color=data_dfs[event_type].date, text=data_dfs[event_type].annotations, hover_data=['times', 'states', 'labels', 'extras'], color_discrete_sequence=px.colors.qualitative.Dark24, markers=True)
        for i in range(len(fig_to_add.data)):
            fig.add_trace(fig_to_add.data[i])
'''
all_data_df = pd.concat(frames)
date_labels = all_data_df["date"].unique()
buttonsDates = [dict(label = "All dates",
                            method="restyle",
                            args=[{'y': [all_data_df[""]]}])]
'''
#CREATE SUBFRAMES BASED ON DATA. 

fig.update_traces(line_shape="hv")

fig.update_xaxes(categoryorder='category ascending')

fig.update_yaxes(
    ticktext=ticktext,
    tickvals=tickvals,
)

fig.update_traces(textfont_size=10)#textposition='bottom center', textfont_size=6)

wlan_figure_annotation="wlan0 Disconnects: "
for key in wlan_disconnect_data:
    wlan_figure_annotation += "<br>{}: {}".format(key, wlan_disconnect_data[key])

fig.add_annotation(text=wlan_figure_annotation, 
                    align='left',
                    showarrow=False,
                    xref='paper',
                    yref='paper',
                    x=1.12,
                    y=0.1,
                    bordercolor='black',
                    borderwidth=1)

fig.update_layout(hoverdistance=1, modebar_add="togglespikelines", legend={'traceorder':'grouped'})

fig.write_html('logcat_figure.html', auto_open=True)

with pd.ExcelWriter('output.xlsx') as writer:
    for df in data_dfs.keys():
        print(df, data_dfs[df])
        data_dfs[df].to_excel(writer, sheet_name=df, engine='openpyxl')