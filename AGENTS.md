# Check AVL Dates

this is a program running as a service on a linux server

update avl ical dates from a public url once a week

check avl ical dates once per hour:

  if there is a past date remove it from the list

  if there is a not acknowledged date within 3h:
  then set urgent color code for wled

  if there is a not acknowledged date within 24h:
  if not night time then set color code for wled

if there is an acknowledge (web request) and codes
then remove oldest dates and color code(s)

while there are color codes
do encode color codes and send to wled

color codes: part all leds into segments:
one for urgent (at least one urgent code) or normal
one for each active color code

