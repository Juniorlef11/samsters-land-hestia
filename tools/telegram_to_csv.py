#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
HESTIA — μετατροπή Telegram export (result.json) -> CSV για το Google Sheet.
Διαβάζει τα /status μηνύματα του bot, βγάζει timestamp + θερμοκρασίες + ισχύ + kWh.
Χρήση:  python telegram_to_csv.py ChatExport_.../result.json history.csv
"""
import json, re, sys, csv

def num(pattern, text):
    m = re.search(pattern, text)
    if not m: return ''
    v = m.group(1)
    return '' if v in ('--', '') else v

def status_code(header):
    h = header
    if 'Αναμονή ζεστ' in h: return 'WAIT_HOT'
    if 'Αναμονή κρύ'  in h: return 'WAIT_COLD'
    if 'Θέρμανση'     in h: return 'HEAT'
    if 'Ψύξη'         in h: return 'COOL'
    if 'ΣΒΗΣΤΟ' in h or 'OFF' in h: return 'OFF'
    if 'Αντιπαγ' in h: return 'FROST'
    if 'Ασφάλ'   in h: return 'SAFETY'
    return 'OK'

def parse(path_json, path_csv):
    d = json.load(open(path_json, encoding='utf-8'))
    rows, seen = [], set()
    total_status = 0
    for m in d.get('messages', []):
        if m.get('from_id') != 'user8937541380':        # μόνο μηνύματα του bot
            continue
        full = ''.join(e.get('text', '') for e in m.get('text_entities', []))
        if 'buffer' not in full or ' W' not in full:     # μόνο status μηνύματα
            continue
        total_status += 1
        ts = m['date'].replace('T', ' ')                 # 2026-06-18 23:12:54
        header = next((ln.strip() for ln in full.splitlines() if ln.strip()), '')
        season = 'summer' if 'Καλοκαίρι' in full else ('winter' if 'Χειμώνας' in full else '')
        row = {
            'ts':       ts,
            'hotTop':   num(r'Hot buffer:\s*(-?\d+\.?\d*|--)', full),
            'outdoor':  num(r'Εξωτερική:\s*(-?\d+\.?\d*|--)', full),
            'cold':     num(r'Cold buffer:\s*(-?\d+\.?\d*|--)', full),
            'supply':   num(r'Τρίοδη:\s*(-?\d+\.?\d*|--)', full),
            'powerW':   num(r'Αντλ[^:]*:\s*(\d+)\s*W', full),
            'room':     num(r'Δωμάτιο:\s*(-?\d+\.?\d*|--)', full),
            'rh':       '',                              # δεν υπάρχει στα παλιά
            'setpoint': num(r'Setpoint:\s*(-?\d+\.?\d*)', full),
            'dew':      '',                              # δεν υπάρχει στα παλιά
            'status':   status_code(header),
            'season':   season,
            'rssi':     num(r'RSSI\s*(-?\d+)\s*dBm', full),
            'kwhDay':   num(r'Σήμερα:\s*(\d+\.?\d*)\s*kWh', full),
        }
        key = (ts,)                                      # dedup ίδιο timestamp
        if key in seen:
            continue
        seen.add(key)
        rows.append(row)

    cols = ['ts','hotTop','outdoor','cold','supply','powerW','room','rh',
            'setpoint','dew','status','season','rssi','kwhDay']
    with open(path_csv, 'w', newline='', encoding='utf-8') as f:
        w = csv.DictWriter(f, fieldnames=cols)
        w.writeheader()
        w.writerows(rows)

    withpow = sum(1 for r in rows if r['powerW'] not in ('', '0'))
    dates = sorted(r['ts'][:10] for r in rows)
    print(f'status messages: {total_status}')
    print(f'unique rows written: {len(rows)}')
    print(f'rows with power>0: {withpow}')
    if dates:
        print(f'date range: {dates[0]} .. {dates[-1]}')

if __name__ == '__main__':
    parse(sys.argv[1], sys.argv[2])
