/*
    This file is part of Kismet

    Kismet is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    Kismet is distributed in the hope that it will be useful,
      but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Kismet; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#include "config.h"

#include "alertracker.h"
#include "configfile.h"
#include "kismet_server.h"

char *ALERT_fields_text[] = {
    "sec", "usec", "header", "bssid", "source", "dest", "other", "channel", "text",
    NULL
};

// alert.  data = ALERT_data
int Protocol_ALERT(PROTO_PARMS) {
    ALERT_data *adata = (ALERT_data *) data;

    for (unsigned int x = 0; x < field_vec->size(); x++) {
        switch ((ALERT_fields) (*field_vec)[x]) {
        case ALERT_header:
            out_string += adata->header;
            break;
        case ALERT_sec:
            out_string += adata->sec;
            break;
        case ALERT_usec:
            out_string += adata->usec;
            break;
        case ALERT_bssid:
            out_string += adata->bssid;
            break;
        case ALERT_source:
            out_string += adata->source;
            break;
        case ALERT_dest:
            out_string += adata->dest;
            break;
        case ALERT_other:
            out_string += adata->other;
            break;
        case ALERT_channel:
            out_string += adata->channel;
            break;
        case ALERT_text:
            out_string += string("\001") + adata->text + string("\001");
            break;
        default:
            out_string = "Unknown field requested.";
            return -1;
            break;
        }

        out_string += " ";
    }

    return 1;
}

void Protocol_ALERT_enable(PROTO_ENABLE_PARMS) {
    globalreg->alertracker->BlitBacklogged(in_fd);
}

Alertracker::Alertracker() {
    fprintf(stderr, "*** Alertracker::Alertracker() called with no global registry.  Bad.\n");
}

Alertracker::Alertracker(GlobalRegistry *in_globalreg) {
    globalreg = in_globalreg;
    next_alert_id = 0;

    if (globalreg->kismet_config->FetchOpt("alertbacklog") != "") {
        int scantmp;
        if (sscanf(globalreg->kismet_config->FetchOpt("alertbacklog").c_str(), 
                   "%d", &scantmp) != 1 || scantmp < 0) {
            globalreg->messagebus->InjectMessage("Illegal value for 'alertbacklog' "
												 "in config file", MSGFLAG_FATAL);
            globalreg->fatal_condition = 1;
            return;
        }
        num_backlog = scantmp;
    }
    
    // Autoreg the alert protocol
    globalreg->alr_prot_ref = 
        globalreg->kisnetserver->RegisterProtocol("ALERT", 0, ALERT_fields_text, 
                                                  &Protocol_ALERT, 
												  &Protocol_ALERT_enable);
}

Alertracker::~Alertracker() {
    for (map<int, alert_rec *>::iterator x = alert_ref_map.begin();
         x != alert_ref_map.end(); ++x)
        delete x->second;
}

int Alertracker::RegisterAlert(const char *in_header, alert_time_unit in_unit, 
							   int in_rate, alert_time_unit in_burstunit,
                               int in_burst) {
	char err[1024];

    // Bail if this header is registered
    if (alert_name_map.find(in_header) != alert_name_map.end()) {
		snprintf(err, 1024, "RegisterAlert() header already registered '%s'",
				 in_header);
		globalreg->messagebus->InjectMessage(err, MSGFLAG_ERROR);
        return -1;
	}

	// Bail if the rates are impossible
	if (in_burstunit > in_unit) {
		snprintf(err, 1024, "RegisterAlert() header '%s' failed, time unit for "
				 "burst rate must be <= time unit for max rate", in_header);
		globalreg->messagebus->InjectMessage(err, MSGFLAG_ERROR);
		return -1;
	}

    alert_rec *arec = new alert_rec;

    arec->ref_index = next_alert_id++;
    arec->header = in_header;
    arec->limit_unit = in_unit;
	arec->burst_unit = in_burstunit;
    arec->limit_rate = in_rate;
    arec->limit_burst = in_burst;
    arec->burst_sent = 0;

    alert_name_map[arec->header] = arec->ref_index;
    alert_ref_map[arec->ref_index] = arec;

    return arec->ref_index;
}

int Alertracker::FetchAlertRef(string in_header) {
    if (alert_name_map.find(in_header) != alert_name_map.end())
        return alert_name_map[in_header];

    return -1;
}

int Alertracker::CheckTimes(alert_rec *arec) {
    // Is this alert rate-limited?  If not, shortcut out and send it
    if (arec->limit_rate == 0) {
        return 1;
    }

    struct timeval now;
    gettimeofday(&now, NULL);

	// If the last time we sent anything was longer than the main rate limit,
	// then we reset back to empty
	if (arec->time_last < (now.tv_sec - alert_time_unit_conv[arec->limit_unit])) {
		arec->total_sent = 0;
		arec->burst_sent = 0;
		return 1;
	}

	// If the last time we sent anything was longer than the burst rate, we can
	// reset the burst to 0
	if (arec->time_last < (now.tv_sec - alert_time_unit_conv[arec->limit_burst])) {
		arec->burst_sent = 0;
	}

	// If we're under the limit on both, we're good to go
	if (arec->burst_sent < arec->limit_burst && arec->total_sent < arec->limit_rate)
		return 1;

    return 0;
}

int Alertracker::PotentialAlert(int in_ref) {
    map<int, alert_rec *>::iterator aritr = alert_ref_map.find(in_ref);

    if (aritr == alert_ref_map.end())
        return -1;

    alert_rec *arec = aritr->second;

    return CheckTimes(arec);
}

int Alertracker::RaiseAlert(int in_ref, 
                            mac_addr bssid, mac_addr source, mac_addr dest, 
							mac_addr other, int in_channel, string in_text) {
    map<int, alert_rec *>::iterator aritr = alert_ref_map.find(in_ref);

    if (aritr == alert_ref_map.end())
        return -1;

    alert_rec *arec = aritr->second;

    if (CheckTimes(arec) != 1)
        return 0;

    ALERT_data *adata = new ALERT_data;

    char tmpstr[128];
    timeval *ts = new timeval;
    gettimeofday(ts, NULL);

    snprintf(tmpstr, 128, "%ld", (long int) ts->tv_sec);
    adata->sec = tmpstr;

    snprintf(tmpstr, 128, "%ld", (long int) ts->tv_usec);
    adata->usec = tmpstr;

    snprintf(tmpstr, 128, "%d", in_channel);
    adata->channel = tmpstr;

    adata->text = in_text;
    adata->header = arec->header;
    adata->bssid = bssid.Mac2String();
    adata->source = source.Mac2String();
    adata->dest  = dest.Mac2String();
    adata->other = other.Mac2String();

	// Increment and set the timers
    arec->burst_sent++;
	arec->total_sent++;
	arec->time_last = time(0);

    alert_backlog.push_back(adata);
    if ((int) alert_backlog.size() > num_backlog) {
        delete alert_backlog[0];
        alert_backlog.erase(alert_backlog.begin());
    }

    globalreg->kisnetserver->SendToAll(globalreg->alr_prot_ref,
                                            (void *) adata);
    
    // Hook main for sounds and whatnot on the server
    globalreg->messagebus->InjectMessage(adata->text, MSGFLAG_ALERT);

    return 1;
}

void Alertracker::BlitBacklogged(int in_fd) {
    for (unsigned int x = 0; x < alert_backlog.size(); x++)
        globalreg->kisnetserver->SendToAll(globalreg->alr_prot_ref, 
                                           (void *) alert_backlog[x]);
        //server->SendToClient(in_fd, protoref, (void *) alert_backlog[x]);
}

int Alertracker::ParseAlertStr(string alert_str, string *ret_name, 
							   alert_time_unit *ret_limit_unit, int *ret_limit_rate,
							   alert_time_unit *ret_limit_burst, 
							   int *ret_burst_rate) {
	char err[1024];
	vector<string> tokens = StrTokenize(alert_str, ",");

	if (tokens.size() != 3) {
		snprintf(err, 1024, "Malformed limits for alert '%s'", alert_str.c_str());
		globalreg->messagebus->InjectMessage(err, MSGFLAG_ERROR);
		return -1;
	}

	(*ret_name) = StrLower(tokens[0]);

	if (ParseRateUnit(StrLower(tokens[1]), ret_limit_unit, ret_limit_rate) != 1 ||
		ParseRateUnit(StrLower(tokens[2]), ret_limit_unit, ret_limit_rate) != 1) {
		snprintf(err, 1024, "Malformed limits for alert '%s'", alert_str.c_str());
		globalreg->messagebus->InjectMessage(err, MSGFLAG_ERROR);
		return -1;
	}

	return 1;
}

// Split up a rate/unit string into real values
int Alertracker::ParseRateUnit(string in_ru, alert_time_unit *ret_unit,
							   int *ret_rate) {
	char err[1024];
	vector<string> units = StrTokenize(in_ru, "/");

	if (units.size() == 1) {
		// Unit is per minute if not specified
		(*ret_unit) = sat_minute;
	} else {
		// Parse the string unit
		if (units[1] == "sec" || units[1] == "second") {
			(*ret_unit) = sat_second;
		} else if (units[1] == "min" || units[1] == "minute") {
			(*ret_unit) = sat_minute;
		} else if (units[1] == "hr" || units[1] == "hour") { 
			(*ret_unit) = sat_hour;
		} else if (units[1] == "day") {
			(*ret_unit) = sat_day;
		} else {
			snprintf(err, 1024, "Alertracker - Invalid time unit for alert rate '%s'",
					 units[1].c_str());
			globalreg->messagebus->InjectMessage(err, MSGFLAG_ERROR);
			return -1;
		}
	}

	// Get the number
	if (sscanf(units[0].c_str(), "%d", ret_rate) != 1) {
		snprintf(err, 1024, "Alertracker - Invalid rate '%s' for alert",
				 units[0].c_str());
		globalreg->messagebus->InjectMessage(err, MSGFLAG_ERROR);
		return -1;
	}

	return 1;
}

