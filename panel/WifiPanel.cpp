/*
 * WifiPanel - a small Wi-Fi control panel for ZETA, for the iwi driver + iwid daemon.
 *
 *   - shows whether Wi-Fi is connected (network, signal, address, router)
 *   - lists the networks in range with signal bars and security
 *   - Connect: asks for the password of a new network (saved to ~/config/settings/iwi.conf) or just switches to a saved one
 *   - Forget: removes a saved network
 *   - Rescan: asks the daemon for a fresh scan (the card is restarted for it: a few seconds offline)
 *
 * It does not touch the card itself.  It only reads /tmp/iwid.status and /tmp/iwid.scan, which iwid writes, and
 * sends commands to iwid by writing one line into /tmp/iwid.cmd.  Copyright (c) 2026 Vihaan Nathan, ISC licence.
 *
 * Plain R5-style C++ for ZETA's gcc 2.95:  c++ -O1 -Wno-multichar -ffor-scope -o WifiPanel WifiPanel.cpp -lbe
 */
#include <Alert.h>
#include <Application.h>
#include <Button.h>
#include <CheckBox.h>
#include <Entry.h>
#include <File.h>
#include <Font.h>
#include <ListItem.h>
#include <ListView.h>
#include <Menu.h>
#include <MenuBar.h>
#include <MenuItem.h>
#include <Message.h>
#include <MessageRunner.h>
#include <OS.h>
#include <ScrollView.h>
#include <String.h>
#include <StringView.h>
#include <TextControl.h>
#include <TextView.h>
#include <View.h>
#include <Window.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wificonf.h"

#ifndef B_UTF8_ELLIPSIS
#define B_UTF8_ELLIPSIS "\xE2\x80\xA6"
#endif
#ifndef B_V_SCROLL_BAR_WIDTH
#define B_V_SCROLL_BAR_WIDTH 14
#endif

static const char* kSignature = "application/x-vnd.ZetaWifi-Panel";
static const char* kStatusFile = "/tmp/iwid.status";
static const char* kScanFile = "/tmp/iwid.scan";
static const char* kCmdFile = "/tmp/iwid.cmd";
static const char* kConfFile = "/boot/home/config/settings/iwi.conf";

enum {
	MSG_TICK = 'wfTk',
	MSG_RESCAN = 'wfRs',
	MSG_CONNECT = 'wfCn',
	MSG_FORGET = 'wfFg',
	MSG_START = 'wfSt',
	MSG_SELECTION = 'wfSl',
	MSG_PASSWORD_OK = 'wfPk',
	MSG_SHOW_PASSWORD = 'wfSh',
	MSG_PASSWORD_CANCEL = 'wfPc',
	MSG_ABOUT = 'wfAb'
};

/* ------------------------------------------------------------------------------------------------------------
 * helpers: files
 * ---------------------------------------------------------------------------------------------------------- */

static bool
ReadWholeFile(const char* path, BString& out)
{
	BFile file(path, B_READ_ONLY);
	char buffer[2048];
	ssize_t n;

	out = "";
	if (file.InitCheck() != B_OK)
		return false;
	while ((n = file.Read(buffer, sizeof(buffer) - 1)) > 0) {
		buffer[n] = '\0';
		out << buffer;
	}
	return true;
}

static bool
WriteWholeFile(const char* path, const BString& text)
{
	BFile file(path, B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);

	if (file.InitCheck() != B_OK)
		return false;
	return file.Write(text.String(), text.Length()) == text.Length();
}

static void
SendCommand(const char* line)
{
	BString text(line);

	text << "\n";
	WriteWholeFile(kCmdFile, text);
}

/* the next "line" of text starting at pos; returns false at the end */
static bool
NextLine(const BString& text, int32& pos, BString& line)
{
	int32 end;

	if (pos >= text.Length())
		return false;
	end = text.FindFirst('\n', pos);
	if (end < 0)
		end = text.Length();
	text.CopyInto(line, pos, end - pos);
	pos = end + 1;
	return true;
}

/* ------------------------------------------------------------------------------------------------------------
 * the saved networks in iwi.conf: pairs of "ssid=" / "psk=" lines (other lines are preserved).
 * The text editing lives in wificonf.h, which has its own tests (test_wificonf.c); before every change the old file is
 * copied to iwi.conf.bak.
 * ---------------------------------------------------------------------------------------------------------- */

static bool
IsKnownNetwork(const char* ssid)
{
	BString text;

	if (!ReadWholeFile(kConfFile, text))
		return false;
	return conf_has_ssid(text.String(), ssid) != 0;
}

static void
BackupConf()
{
	BString text;

	if (ReadWholeFile(kConfFile, text) && text.Length() > 0)
		WriteWholeFile("/boot/home/config/settings/iwi.conf.bak", text);
}

/* add or replace a network; returns false if the file could not be written */
static bool
SaveNetwork(const char* ssid, const char* psk)
{
	BString text;
	int size, n;
	char* buffer;
	bool ok;

	ReadWholeFile(kConfFile, text);
	size = text.Length() + (int)strlen(ssid) + (int)strlen(psk) + 64;
	buffer = new char[size];
	n = conf_set_network(text.String(), ssid, psk, buffer, size);
	if (n < 0) {
		delete[] buffer;
		return false;
	}
	BackupConf();
	ok = WriteWholeFile(kConfFile, BString(buffer, n));
	delete[] buffer;
	return ok;
}

static bool
ForgetNetwork(const char* ssid)
{
	BString text;
	int size, n;
	char* buffer;
	bool ok;

	if (!ReadWholeFile(kConfFile, text))
		return false;
	size = text.Length() + 64;
	buffer = new char[size];
	n = conf_forget_network(text.String(), ssid, buffer, size);
	if (n < 0) {
		delete[] buffer;
		return false;
	}
	BackupConf();
	ok = WriteWholeFile(kConfFile, BString(buffer, n));
	delete[] buffer;
	return ok;
}

/* ------------------------------------------------------------------------------------------------------------
 * one network in the list
 * ---------------------------------------------------------------------------------------------------------- */

static int
SignalBars(int dbm)
{
	if (dbm >= -55)
		return 4;
	if (dbm >= -65)
		return 3;
	if (dbm >= -75)
		return 2;
	if (dbm >= -85)
		return 1;
	return 0;
}

static const char*
SignalWord(int dbm)
{
	if (dbm >= -50)
		return "excellent";
	if (dbm >= -60)
		return "good";
	if (dbm >= -70)
		return "fair";
	return "weak";
}

static const char*
SecurityName(int sec)
{
	switch (sec) {
		case 3:
			return "WPA2";
		case 2:
			return "WPA";
		case 1:
			return "WEP";
	}
	return "open";
}

class NetworkItem : public BListItem {
public:
	NetworkItem(const char* ssid, int dbm, int sec, int chan, bool known, bool connected)
		:
		BListItem(),
		fSsid(ssid),
		fDbm(dbm),
		fSec(sec),
		fChan(chan),
		fKnown(known),
		fConnected(connected)
	{
	}

	const char* Ssid() const { return fSsid.String(); }
	int Dbm() const { return fDbm; }
	int Security() const { return fSec; }
	bool Known() const { return fKnown; }
	bool Connected() const { return fConnected; }
	bool Supported() const { return fSec == 3; }	/* the daemon implements WPA2 (AES) only */

	virtual void Update(BView* owner, const BFont* font)
	{
		font_height fh;

		BListItem::Update(owner, font);
		font->GetHeight(&fh);
		SetHeight(fh.ascent + fh.descent + fh.leading + 12);
	}

	virtual void DrawItem(BView* owner, BRect frame, bool complete)
	{
		rgb_color background = { 255, 255, 255, 255 };
		rgb_color selected = { 200, 218, 245, 255 };
		rgb_color black = { 0, 0, 0, 255 };
		rgb_color gray = { 130, 130, 130, 255 };
		rgb_color bar = { 40, 110, 200, 255 };
		rgb_color barOff = { 215, 215, 215, 255 };
		font_height fh;
		BFont font;
		int bars = SignalBars(fDbm), i;
		float x = frame.left + 8, y = frame.bottom - 4;
		BString right;

		if (IsSelected())
			owner->SetLowColor(selected);
		else
			owner->SetLowColor(background);
		owner->FillRect(frame, B_SOLID_LOW);

		/* signal bars, 4 of them, growing */
		for (i = 0; i < 4; i++) {
			BRect r(x + i * 5, frame.bottom - 6 - (i + 1) * 3, x + i * 5 + 3, frame.bottom - 6);
			owner->SetHighColor(Supported() && i < bars ? bar : (i < bars ? gray : barOff));
			owner->FillRect(r);
		}

		owner->GetFont(&font);
		font.GetHeight(&fh);
		y = frame.top + (frame.Height() + fh.ascent - fh.descent) / 2;

		/* name (bold when connected, grey when we cannot use the network) */
		BFont bold(font);
		if (fConnected)
			bold.SetFace(B_BOLD_FACE);
		owner->SetFont(&bold);
		owner->SetHighColor(Supported() ? black : gray);
		owner->DrawString(fSsid.String(), BPoint(x + 28, y));
		owner->SetFont(&font);

		/* right side: connected / saved, security */
		right = "";
		if (fConnected)
			right << "connected   ";
		else if (fKnown)
			right << "saved   ";
		right << SecurityName(fSec);
		owner->SetHighColor(gray);
		owner->DrawString(right.String(), BPoint(frame.right - 8 - font.StringWidth(right.String()), y));
	}

private:
	BString fSsid;
	int fDbm;
	int fSec;
	int fChan;
	bool fKnown;
	bool fConnected;
};

/* ------------------------------------------------------------------------------------------------------------
 * the password window
 * ---------------------------------------------------------------------------------------------------------- */

class PasswordWindow : public BWindow {
public:
	PasswordWindow(BMessenger target, const char* ssid, BRect around)
		:
		BWindow(BRect(around.left + 30, around.top + 80, around.left + 370, around.top + 215), "Connect",
			B_TITLED_WINDOW, B_NOT_RESIZABLE | B_NOT_ZOOMABLE),
		fTarget(target),
		fSsid(ssid)
	{
		BRect b = Bounds();
		BView* view = new BView(b, "back", B_FOLLOW_ALL, B_WILL_DRAW);
		BString title("Password for \"");

		title << ssid << "\"";
		view->SetViewColor(216, 216, 216);
		AddChild(view);
		view->AddChild(new BStringView(BRect(12, 10, b.right - 12, 28), "title", title.String()));
		fPassword = new BTextControl(BRect(12, 38, b.right - 12, 58), "password", "Password:", "", NULL);
		fPassword->SetDivider(70);
		fPassword->TextView()->HideTyping(true);
		view->AddChild(fPassword);
		fShow = new BCheckBox(BRect(82, 64, b.right - 12, 82), "show", "Show password", new BMessage(MSG_SHOW_PASSWORD));
		view->AddChild(fShow);
		BButton* cancel = new BButton(BRect(b.right - 180, b.bottom - 36, b.right - 100, b.bottom - 12), "cancel", "Cancel",
			new BMessage(MSG_PASSWORD_CANCEL));
		BButton* ok = new BButton(BRect(b.right - 90, b.bottom - 36, b.right - 12, b.bottom - 12), "ok", "Connect",
			new BMessage(MSG_PASSWORD_OK));
		view->AddChild(cancel);
		view->AddChild(ok);
		ok->MakeDefault(true);
		fPassword->MakeFocus(true);
	}

	virtual void MessageReceived(BMessage* message)
	{
		switch (message->what) {
			case MSG_SHOW_PASSWORD:
				fPassword->TextView()->HideTyping(fShow->Value() == 0);
				fPassword->TextView()->Invalidate();
				break;

			case MSG_PASSWORD_CANCEL:
				PostMessage(B_QUIT_REQUESTED);
				break;

			case MSG_PASSWORD_OK:
			{
				BMessage reply(MSG_PASSWORD_OK);
				const char* psk = fPassword->Text();

				if (strlen(psk) < 8 || strlen(psk) > 63) {
					(new BAlert("Password", "A WPA2 password has 8 to 63 characters.", "OK"))->Go(NULL);
					break;
				}
				if (strchr(psk, '\n') != NULL || strchr(psk, '\r') != NULL)
					break;
				reply.AddString("ssid", fSsid.String());
				reply.AddString("psk", psk);
				fTarget.SendMessage(&reply);
				PostMessage(B_QUIT_REQUESTED);
				break;
			}

			default:
				BWindow::MessageReceived(message);
		}
	}

private:
	BMessenger fTarget;
	BString fSsid;
	BTextControl* fPassword;
	BCheckBox* fShow;
};

/* ------------------------------------------------------------------------------------------------------------
 * the main window
 * ---------------------------------------------------------------------------------------------------------- */

struct WifiStatus {
	BString state, error, ssid, bssid, ip, router, dns;
	int chan, dbm;
	long updated;
	bool valid;
	WifiStatus() : chan(0), dbm(0), updated(0), valid(false) {}
};

static void
ReadStatus(WifiStatus& st)
{
	BString text, line;
	int32 pos = 0;

	st = WifiStatus();
	if (!ReadWholeFile(kStatusFile, text))
		return;
	while (NextLine(text, pos, line)) {
		int32 eq = line.FindFirst('=');
		BString key, value;

		if (eq < 0)
			continue;
		line.CopyInto(key, 0, eq);
		line.CopyInto(value, eq + 1, line.Length() - eq - 1);
		if (key == "state")
			st.state = value;
		else if (key == "error")
			st.error = value;
		else if (key == "ssid")
			st.ssid = value;
		else if (key == "bssid")
			st.bssid = value;
		else if (key == "ip")
			st.ip = value;
		else if (key == "router")
			st.router = value;
		else if (key == "dns")
			st.dns = value;
		else if (key == "chan")
			st.chan = atoi(value.String());
		else if (key == "dbm")
			st.dbm = atoi(value.String());
		else if (key == "updated")
			st.updated = atol(value.String());
	}
	st.valid = st.state.Length() > 0;
}

class WifiWindow : public BWindow {
public:
	WifiWindow()
		:
		BWindow(BRect(120, 90, 500, 520), "Wi-Fi", B_TITLED_WINDOW, B_ASYNCHRONOUS_CONTROLS),
		fRunner(NULL),
		fScanTime(0),
		fSelectedSsid(""),
		fLastAlive(false)
	{
		BRect b = Bounds();
		BMenuBar* menuBar = new BMenuBar(BRect(0, 0, b.right, 18), "menubar");
		BMenu* menu = new BMenu("Wi-Fi");

		menu->AddItem(new BMenuItem("Rescan", new BMessage(MSG_RESCAN), 'R'));
		menu->AddItem(new BMenuItem("Connect" B_UTF8_ELLIPSIS, new BMessage(MSG_CONNECT), 'K'));
		menu->AddItem(new BMenuItem("Forget saved network", new BMessage(MSG_FORGET)));
		menu->AddSeparatorItem();
		menu->AddItem(new BMenuItem("Start Wi-Fi service", new BMessage(MSG_START)));
		menu->AddSeparatorItem();
		menu->AddItem(new BMenuItem("About" B_UTF8_ELLIPSIS, new BMessage(MSG_ABOUT)));
		menu->AddItem(new BMenuItem("Quit", new BMessage(B_QUIT_REQUESTED), 'Q'));
		menuBar->AddItem(menu);
		AddChild(menuBar);

		float top = menuBar->Frame().bottom + 1;
		BView* back = new BView(BRect(0, top, b.right, b.bottom), "back", B_FOLLOW_ALL, B_WILL_DRAW);
		back->SetViewColor(216, 216, 216);
		AddChild(back);
		BRect bb = back->Bounds();

		fTitle = new BStringView(BRect(12, 10, bb.right - 12, 32), "title", "");
		BFont big(be_bold_font);
		big.SetSize(be_bold_font->Size() + 4);
		fTitle->SetFont(&big);
		back->AddChild(fTitle);
		fLine2 = new BStringView(BRect(12, 36, bb.right - 12, 52), "line2", "");
		back->AddChild(fLine2);
		fLine3 = new BStringView(BRect(12, 54, bb.right - 12, 70), "line3", "");
		back->AddChild(fLine3);
		fError = new BStringView(BRect(12, 72, bb.right - 12, 88), "error", "");
		fError->SetHighColor(190, 30, 30);
		back->AddChild(fError);

		BRect listRect(12, 98, bb.right - 12 - B_V_SCROLL_BAR_WIDTH, bb.bottom - 78);
		fList = new BListView(listRect, "networks", B_SINGLE_SELECTION_LIST, B_FOLLOW_ALL);
		fList->SetSelectionMessage(new BMessage(MSG_SELECTION));
		fList->SetInvocationMessage(new BMessage(MSG_CONNECT));
		back->AddChild(new BScrollView("scroll", fList, B_FOLLOW_ALL, 0, false, true));

		float by = bb.bottom - 66;
		fHint = new BStringView(BRect(12, bb.bottom - 74, bb.right - 12, bb.bottom - 58), "hint",
			"Rescan briefly interrupts the connection.");
		fHint->SetHighColor(110, 110, 110);
		fHint->SetResizingMode(B_FOLLOW_BOTTOM | B_FOLLOW_LEFT);
		back->AddChild(fHint);
		fRescan = new BButton(BRect(12, by + 20, 102, by + 44), "rescan", "Rescan", new BMessage(MSG_RESCAN));
		fConnect = new BButton(BRect(110, by + 20, 220, by + 44), "connect", "Connect" B_UTF8_ELLIPSIS,
			new BMessage(MSG_CONNECT));
		fForget = new BButton(BRect(228, by + 20, 318, by + 44), "forget", "Forget", new BMessage(MSG_FORGET));
		fStart = new BButton(BRect(12, by + 20, 150, by + 44), "start", "Start Wi-Fi service", new BMessage(MSG_START));
		fRescan->SetResizingMode(B_FOLLOW_BOTTOM | B_FOLLOW_LEFT);
		fConnect->SetResizingMode(B_FOLLOW_BOTTOM | B_FOLLOW_LEFT);
		fForget->SetResizingMode(B_FOLLOW_BOTTOM | B_FOLLOW_LEFT);
		fStart->SetResizingMode(B_FOLLOW_BOTTOM | B_FOLLOW_LEFT);
		back->AddChild(fRescan);
		back->AddChild(fConnect);
		back->AddChild(fForget);
		back->AddChild(fStart);
		fStart->Hide();

		SetSizeLimits(330, 900, 300, 900);
		UpdateButtons();
		Refresh(true);
		fRunner = new BMessageRunner(BMessenger(this), new BMessage(MSG_TICK), 2000000);
	}

	~WifiWindow()
	{
		delete fRunner;
	}

	virtual bool QuitRequested()
	{
		be_app->PostMessage(B_QUIT_REQUESTED);
		return true;
	}

	virtual void MessageReceived(BMessage* message)
	{
		switch (message->what) {
			case MSG_TICK:
				Refresh(false);
				break;

			case MSG_SELECTION:
				RememberSelection();
				UpdateButtons();
				break;

			case MSG_RESCAN:
				SendCommand("scan");
				fError->SetText("");
				fTitle->SetText("Scanning...");
				fLine2->SetText("The card restarts for a clean scan; this takes a few seconds.");
				fLine3->SetText("");
				break;

			case MSG_CONNECT:
				ConnectSelected();
				break;

			case MSG_PASSWORD_OK:
			{
				const char* ssid;
				const char* psk;
				if (message->FindString("ssid", &ssid) == B_OK && message->FindString("psk", &psk) == B_OK) {
					if (!SaveNetwork(ssid, psk)) {
						(new BAlert("Wi-Fi", "Could not write the Wi-Fi settings file.", "OK"))->Go(NULL);
						break;
					}
					BString cmd("connect ");
					cmd << ssid;
					SendCommand(cmd.String());
					fError->SetText("");
					fTitle->SetText("Connecting...");
					fLine2->SetText(ssid);
					fLine3->SetText("");
				}
				break;
			}

			case MSG_FORGET:
			{
				NetworkItem* item = SelectedItem();
				if (item == NULL || !item->Known())
					break;
				BString ask("Forget the saved network \"");
				ask << item->Ssid() << "\"?";
				BAlert* alert = new BAlert("Forget", ask.String(), "Cancel", "Forget", NULL, B_WIDTH_AS_USUAL, B_WARNING_ALERT);
				if (alert->Go() == 1) {
					ForgetNetwork(item->Ssid());
					SendCommand("reload");
					fSelectedSsid = "";
					Refresh(true);
				}
				break;
			}

			case MSG_START:
				system("/bin/sh -c '/boot/home/ZetaWifi/start-wifi.sh > /dev/null 2>&1 &'");
				fTitle->SetText("Starting the Wi-Fi service...");
				break;

			case MSG_ABOUT:
				(new BAlert("About", "Wi-Fi panel for the iwi driver and iwid daemon.\n\n"
					"It lists the networks in range and tells the daemon which one to use.\n"
					"WPA2 (AES) networks only.", "OK"))->Go(NULL);
				break;

			default:
				BWindow::MessageReceived(message);
		}
	}

private:
	NetworkItem* SelectedItem()
	{
		int32 index = fList->CurrentSelection();

		if (index < 0)
			return NULL;
		return dynamic_cast<NetworkItem*>(fList->ItemAt(index));
	}

	void RememberSelection()
	{
		NetworkItem* item = SelectedItem();

		if (item != NULL)
			fSelectedSsid = item->Ssid();
	}

	void UpdateButtons()
	{
		NetworkItem* item = SelectedItem();

		fConnect->SetEnabled(item != NULL && item->Supported() && !item->Connected());
		fForget->SetEnabled(item != NULL && item->Known());
	}

	void ConnectSelected()
	{
		NetworkItem* item = SelectedItem();

		if (item == NULL)
			return;
		if (!item->Supported()) {
			(new BAlert("Wi-Fi", "Only WPA2 (AES) networks are supported by this driver.", "OK"))->Go(NULL);
			return;
		}
		if (item->Connected())
			return;
		if (item->Known()) {
			BString text("\"");
			text << item->Ssid() << "\" is a saved network.";
			BAlert* alert = new BAlert("Connect", text.String(), "Cancel", "Change password" B_UTF8_ELLIPSIS, "Connect");
			int32 which = alert->Go();

			if (which == 2) {
				BString cmd("connect ");
				cmd << item->Ssid();
				SendCommand(cmd.String());
				fTitle->SetText("Connecting...");
				fLine2->SetText(item->Ssid());
				fLine3->SetText("");
				fError->SetText("");
			} else if (which == 1)
				(new PasswordWindow(BMessenger(this), item->Ssid(), Frame()))->Show();
			return;
		}
		(new PasswordWindow(BMessenger(this), item->Ssid(), Frame()))->Show();
	}

	void Refresh(bool force)
	{
		WifiStatus st;
		long now = (long)(system_time() / 1000000);
		bool alive;
		BString line;

		ReadStatus(st);
		alive = st.valid && (now - st.updated) <= 8 && (now >= st.updated);
		if (!alive) {
			fTitle->SetText("Wi-Fi service is not running");
			fLine2->SetText("Press \"Start Wi-Fi service\" (or reboot).");
			fLine3->SetText("");
			fError->SetText("");
			fStart->Show();
			fRescan->SetEnabled(false);
			fConnect->SetEnabled(false);
			fForget->SetEnabled(false);
		} else {
			if (!fStart->IsHidden())
				fStart->Hide();
			fRescan->SetEnabled(true);
			ShowState(st);
			UpdateButtons();
		}
		ReloadList(st, alive, force);
	}

	void ShowState(const WifiStatus& st)
	{
		BString line;
		char buf[160];

		fError->SetText(st.error.String());
		if (st.state == "connected") {
			line = "Connected to ";
			line << st.ssid;
			fTitle->SetText(line.String());
			if (st.ip.Length() > 0) {
				line = st.ip;
				line << "   via " << st.router;
				if (st.dns.Length() > 0)
					line << "   DNS " << st.dns;
			} else
				line = "No address yet";
			fLine2->SetText(line.String());
			sprintf(buf, "Signal %d dBm (%s)   channel %d", st.dbm, SignalWord(st.dbm), st.chan);
			fLine3->SetText(buf);
		} else if (st.state == "scanning") {
			fTitle->SetText("Scanning...");
			fLine2->SetText("");
			fLine3->SetText("");
		} else if (st.state == "connecting") {
			fTitle->SetText("Connecting...");
			fLine2->SetText("");
			fLine3->SetText("");
		} else if (st.state == "reconnecting" || st.state == "restarting") {
			fTitle->SetText("Reconnecting...");
			fLine2->SetText("");
			fLine3->SetText("");
		} else if (st.state == "starting") {
			fTitle->SetText("Starting...");
			fLine2->SetText("");
			fLine3->SetText("");
		} else {
			fTitle->SetText("Not connected");
			fLine2->SetText("");
			fLine3->SetText("");
		}
	}

	/* rebuild the list from /tmp/iwid.scan when it changed (or on demand) */
	void ReloadList(const WifiStatus& st, bool alive, bool force)
	{
		BEntry entry(kScanFile);
		time_t modified = 0;
		BString text, line;
		int32 pos = 0, i;

		if (entry.InitCheck() == B_OK)
			entry.GetModificationTime(&modified);
		if (!force && modified == fScanTime && alive == fLastAlive && st.ssid == fLastConnected)
			return;
		fScanTime = modified;
		fLastAlive = alive;
		fLastConnected = st.ssid;

		while (fList->CountItems() > 0)
			delete fList->RemoveItem((int32)0);
		if (!ReadWholeFile(kScanFile, text))
			return;

		/* one entry per network name, strongest access point wins; hidden networks (no name) are skipped */
		BList names;
		while (NextLine(text, pos, line)) {
			int chan, dbm, sec;
			char bssid[24], name[80];
			const char* p = line.String();
			const char* bar;
			int field = 0;

			/* channel|dbm|security|bssid|ssid  (the name may contain anything except '|' and newlines) */
			if (sscanf(p, "%d|%d|%d|%23[^|]|", &chan, &dbm, &sec, bssid) != 4)
				continue;
			bar = p;
			while (field < 4 && (bar = strchr(bar, '|')) != NULL) {
				bar++;
				field++;
			}
			if (field != 4 || *bar == '\0')
				continue;
			strncpy(name, bar, sizeof(name) - 1);
			name[sizeof(name) - 1] = '\0';

			NetworkItem* existing = NULL;
			for (i = 0; i < names.CountItems(); i++) {
				NetworkItem* it = (NetworkItem*)names.ItemAt(i);
				if (strcmp(it->Ssid(), name) == 0) {
					existing = it;
					break;
				}
			}
			bool connected = alive && st.state == "connected" && st.ssid == name;
			if (existing != NULL) {
				if (dbm > existing->Dbm()) {
					names.RemoveItem(existing);
					delete existing;
				} else
					continue;
			}
			names.AddItem(new NetworkItem(name, dbm, sec, chan, IsKnownNetwork(name), connected));
		}

		/* order: connected, saved, then by signal */
		int count = names.CountItems(), j;
		for (i = 0; i < count; i++) {
			for (j = i + 1; j < count; j++) {
				NetworkItem* a = (NetworkItem*)names.ItemAt(i);
				NetworkItem* b = (NetworkItem*)names.ItemAt(j);
				int ra = (a->Connected() ? 2000 : 0) + (a->Known() ? 1000 : 0) + (a->Dbm() + 200);
				int rb = (b->Connected() ? 2000 : 0) + (b->Known() ? 1000 : 0) + (b->Dbm() + 200);
				if (rb > ra) {
					names.RemoveItem(j);
					names.RemoveItem(i);
					names.AddItem(b, i);
					names.AddItem(a, j);
				}
			}
		}
		for (i = 0; i < count; i++) {
			NetworkItem* it = (NetworkItem*)names.ItemAt(i);
			fList->AddItem(it);
			if (fSelectedSsid.Length() > 0 && fSelectedSsid == it->Ssid())
				fList->Select(i);
		}
		UpdateButtons();
	}

	BMessageRunner* fRunner;
	BStringView* fTitle;
	BStringView* fLine2;
	BStringView* fLine3;
	BStringView* fError;
	BStringView* fHint;
	BListView* fList;
	BButton* fRescan;
	BButton* fConnect;
	BButton* fForget;
	BButton* fStart;
	time_t fScanTime;
	BString fSelectedSsid;
	BString fLastConnected;
	bool fLastAlive;
};

class WifiApp : public BApplication {
public:
	WifiApp() : BApplication(kSignature) {}

	virtual void ReadyToRun()
	{
		(new WifiWindow())->Show();
	}
};

int
main()
{
	WifiApp app;

	app.Run();
	return 0;
}
