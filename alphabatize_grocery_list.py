#!/usr/bin/env python3
import datetime
import fortune
import getpass
import gkeepapi
import sys
from cryptography.fernet import Fernet

_ENCRYPTED_STRING = b'redacted'

user = "redacted"

keep = gkeepapi.Keep()
with open("/home/pi/programming/the_token_file", "rb") as f:
  token = f.read().decode()

logged_in = False

if token:
  print("Authenticating with token")
  try:
    keep.resume(user, token, sync=False)
    logged_in = True
    print("Success")
  except gkeepapi.exception.LoginException as e:
    print(e)

if not logged_in:
  key = open("/home/pi/programming/the_key_file", "rb").read()
  pwd = Fernet(key).decrypt(_ENCRYPTED_STRING).decode()
  success = keep.login(user, pwd, sync=False)
  del pwd
  if not success:
    print("Failed to log in.")
    sys.exit(1)
  with open("/home/pi/programming/the_token_file", "wb") as f:
    f.write(keep.getMasterToken().encode())

keep.sync()

notes = keep.all()
for i, note in enumerate(notes):
  if note.title == "Grocery List":
    break

note = notes[i]

print("Alphabatizing '{}'".format(note.title))

def Alphabatize(keep_list):
  text_list = [x.text for x in keep_list]
  text_list.sort()
  for i, text in enumerate(text_list):
    keep_list[i].text = text

Alphabatize(note.checked)
Alphabatize(note.unchecked)

# Get your fortune for the day
create_new = True
for i, note in enumerate(notes):
  if note.title.startswith("Fortune") and not note.deleted and not note.trashed:
    create_new = False
    break

if create_new:
  note = keep.createNote('Fortune dummy title', 'dummy text')
  note.pinned = True

title = "Fortune {}".format(datetime.datetime.now().strftime("%d-%m-%y"))

if note.title != title:
  note.title = title
  note.text = fortune.get_random_fortune("/usr/share/games/fortunes/platitudes")


keep.sync()
