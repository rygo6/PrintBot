#ifndef slic3r_GUI_HintNotification_hpp_
#define slic3r_GUI_HintNotification_hpp_

#include "NotificationManager.hpp"

namespace Slic3r {namespace GUI {

// Database of hints updatable
struct HintData
{
	std::string        id_string;
	std::string        key;
	std::string        text;
	size_t			   weight;
	bool               was_displayed;
	std::string        hypertext;
	std::string		   follow_text;
	std::string		   disabled_tags;
	std::string        enabled_tags;
	bool               runtime_disable; // if true - hyperlink will check before every click if not in disabled mode
	std::string        documentation_link;
	std::string        image_url;
	bool               internal_only{ false };
	std::function<void(void)> callback{ nullptr };
};

enum class HintDataNavigation {
	Curr,
	Prev,
	Next,
	Random,
};

class HintDatabase
{
public:
	static HintDatabase& get_instance()
	{
		static HintDatabase    instance; // Guaranteed to be destroyed.
											// Instantiated on first use.
		return instance;
	}
private:
	HintDatabase()
		: m_hint_id(0)
	{}
public:
	~HintDatabase();
	HintDatabase(HintDatabase const&) = delete;
	void operator=(HintDatabase const&) = delete;

	// return true if HintData filled;
	HintData* get_hint(HintDataNavigation nav);
	HintData* get_hint_by_key(const std::string& key);
	size_t	  get_index() { return m_hint_id; }
	size_t    get_count() {
		if (!m_initialized)
			return 0;
		return m_loaded_hints.size();
	}
	// resets m_initiailized to false and writes used if was initialized
	// used when reloading in runtime - like change language
	void    uninit();
	void	reinit();
private:
	void	init();
	void	init_random_hint_id();
	void	load_hints_from_file(const boost::filesystem::path& path, std::vector<HintData>& hints_vector);
	// Returns position in m_loaded_hints with next hint chosed randomly with weights
	//size_t  get_random_next();
	bool						m_initialized{ false };

	size_t						m_hint_id;
	std::vector<HintData>       m_loaded_hints;
	//bool						m_sorted_hints{ false };
};

} //namespace Slic3r 
} //namespace GUI 

#endif //slic3r_GUI_HintNotification_hpp_