#include "PrintBotApproval.hpp"
#include "I18N.hpp"

#include <algorithm>
#include <atomic>

#include <boost/log/trivial.hpp>

#include <wx/button.h>
#include <wx/dialog.h>
#include <wx/font.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/timer.h>

namespace Slic3r { namespace GUI { namespace pbmcp {

namespace {

std::atomic<bool> g_open{false};
wxWindow         *g_dialog = nullptr; // main thread only

class PrintApprovalDialog : public wxDialog
{
public:
    PrintApprovalDialog(wxWindow *parent, const wxString &summary, const std::string &fingerprint, int timeout_s)
        : wxDialog(parent, wxID_ANY, _L("print_bot: approve this print?"), wxDefaultPosition, wxDefaultSize,
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER | wxSTAY_ON_TOP)
        , m_arm_timer(this)
        , m_timeout_timer(this)
    {
        auto *top  = new wxBoxSizer(wxVERTICAL);
        auto *head = new wxStaticText(this, wxID_ANY,
                                      _L("An AI agent asks to send this print job to a printer.\n"
                                        "Check every line. Only your click on \"Approve and send\" starts it; the agent cannot answer this dialog."));
        wxFont bold = head->GetFont();
        bold.SetWeight(wxFONTWEIGHT_BOLD);
        head->SetFont(bold);
        top->Add(head, 0, wxALL | wxEXPAND, 12);

        auto  *text = new wxTextCtrl(this, wxID_ANY, summary, wxDefaultPosition, wxSize(720, 420), wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP);
        wxFont mono(wxFontInfo(10).Family(wxFONTFAMILY_TELETYPE));
        text->SetFont(mono);
        top->Add(text, 1, wxLEFT | wxRIGHT | wxEXPAND, 12);

        top->Add(new wxStaticText(this, wxID_ANY,
                                  wxString::Format(_L("Approval is single-use and bound to job fingerprint %s. If anything changes before sending, "
                                                     "the send is refused and a new approval is needed. This dialog closes without sending after %d minutes."),
                                                   wxString::FromUTF8(fingerprint.substr(0, 16).c_str()), std::max(1, timeout_s / 60))),
                 0, wxALL | wxEXPAND, 12);

        auto *btns = new wxBoxSizer(wxHORIZONTAL);
        m_cancel   = new wxButton(this, wxID_CANCEL, _L("Cancel (do not print)"));
        m_approve  = new wxButton(this, wxID_ANY, _L("Approve and send"));
        m_approve->Enable(false); // armed after a short delay, so a click aimed elsewhere cannot approve
        btns->AddStretchSpacer();
        btns->Add(m_cancel, 0, wxRIGHT, 12);
        btns->Add(m_approve, 0);
        top->Add(btns, 0, wxALL | wxEXPAND, 12);
        SetSizerAndFit(top);
        SetEscapeId(wxID_CANCEL);
        SetAffirmativeId(wxID_NONE); // Enter must not approve by default
        m_cancel->SetDefault();
        m_cancel->SetFocus();
        CentreOnScreen();

        // Real input on the Approve button itself: the press or key must reach this window from
        // the window system. A synthesized wxEVT_BUTTON alone is not enough.
        m_approve->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent &e) {
            if (m_approve->IsEnabled()) m_real_input = true;
            e.Skip();
        });
        m_approve->Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent &e) {
            const int k = e.GetKeyCode();
            if (m_approve->IsEnabled() && (k == WXK_SPACE || k == WXK_RETURN || k == WXK_NUMPAD_ENTER)) m_real_input = true;
            e.Skip();
        });
        m_approve->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
            if (!m_real_input) {
                BOOST_LOG_TRIVIAL(warning) << "print_bot MCP: print approval ignored a button event without real user input";
                return;
            }
            m_approved = true;
            EndModal(wxID_OK);
        });
        Bind(wxEVT_TIMER, [this](wxTimerEvent &e) {
            if (e.GetTimer().GetId() == m_arm_timer.GetId()) {
                m_approve->Enable(true);
            } else if (IsModal()) {
                BOOST_LOG_TRIVIAL(info) << "print_bot MCP: print approval timed out";
                EndModal(wxID_CANCEL);
            }
        });
        m_arm_timer.StartOnce(1500);
        m_timeout_timer.StartOnce(std::max(30, timeout_s) * 1000);
    }

    bool approved() const { return m_approved; }

private:
    wxButton *m_cancel     = nullptr;
    wxButton *m_approve    = nullptr;
    bool      m_real_input = false;
    bool      m_approved   = false;
    wxTimer   m_arm_timer;
    wxTimer   m_timeout_timer;
};

} // namespace

bool approval_dialog_open() { return g_open.load(); }

bool is_approval_window(wxWindow *w)
{
    for (; w && g_dialog; w = w->GetParent())
        if (w == g_dialog) return true;
    return false;
}

bool run_print_approval_dialog(wxWindow *parent, const wxString &summary, const std::string &fingerprint, int timeout_s)
{
    if (g_open.exchange(true)) return false; // one approval at a time
    bool approved = false;
    {
        PrintApprovalDialog dlg(parent, summary, fingerprint, timeout_s);
        g_dialog = &dlg;
        BOOST_LOG_TRIVIAL(info) << "print_bot MCP: waiting for the human to approve print job " << fingerprint.substr(0, 16);
        const int rc = dlg.ShowModal();
        approved     = rc == wxID_OK && dlg.approved();
        g_dialog     = nullptr;
    }
    g_open = false;
    BOOST_LOG_TRIVIAL(info) << "print_bot MCP: print job " << fingerprint.substr(0, 16) << (approved ? " approved by the user" : " not approved");
    return approved;
}

}}} // namespace Slic3r::GUI::pbmcp
