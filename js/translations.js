
const translations = {
    en: {
        title: "Mass Effect 1 Port for Android",
        description: "Experience the legendary universe of Mass Effect 1, now brought to Android. Explore distant worlds, make impactful decisions, build your team, and follow Commander Shepard on an unforgettable journey to save the galaxy. The adventure begins in the palm of your hand.",

        download: "Download",
        cache: "Game Cache",
        documentation: "Documentation"
    },

    ru: {
        title: "Mass Effect 1 — порт для Android",
        description: "Откройте для себя легендарную вселенную Mass Effect 1, теперь на Android. Исследуйте далёкие миры, принимайте судьбоносные решения, собирайте команду и отправляйтесь вместе с капитаном Шепардом в незабываемое путешествие, чтобы спасти галактику. Великое приключение теперь у вас на ладони.",

        download: "Скачать",
        cache: "Кэш игры",
        documentation: "Документация"
    },

    de: {
        title: "Mass Effect 1 Port für Android",
        description: "Erlebe das legendäre Universum von Mass Effect 1 jetzt auf Android. Erkunde ferne Welten, triff bedeutende Entscheidungen, stelle dein Team zusammen und begleite Commander Shepard auf einer unvergesslichen Reise, um die Galaxie zu retten. Das Abenteuer beginnt direkt in deiner Hand.",

        download: "Herunterladen",
        cache: "Spiel-Cache",
        documentation: "Dokumentation"
    },

    fr: {
        title: "Mass Effect 1 sur Android",
        description: "Découvrez l'univers légendaire de Mass Effect 1, désormais sur Android. Explorez des mondes lointains, prenez des décisions décisives, formez votre équipe et accompagnez le commandant Shepard dans une aventure inoubliable pour sauver la galaxie. L'aventure commence au creux de votre main.",

        download: "Télécharger",
        cache: "Cache du jeu",
        documentation: "Documentation"
    },

    it: {
        title: "Mass Effect 1 Port per Android",
        description: "Scopri il leggendario universo di Mass Effect 1, ora disponibile su Android. Esplora mondi lontani, prendi decisioni importanti, forma la tua squadra e accompagna il comandante Shepard in un viaggio indimenticabile per salvare la galassia. L'avventura inizia nel palmo della tua mano.",

        download: "Scarica",
        cache: "Cache del gioco",
        documentation: "Documentazione"
    },

    es: {
        title: "Mass Effect 1 para Android",
        description: "Descubre el legendario universo de Mass Effect 1, ahora en Android. Explora mundos lejanos, toma decisiones trascendentales, reúne a tu equipo y acompaña al comandante Shepard en un viaje inolvidable para salvar la galaxia. La aventura comienza en la palma de tu mano.",

        download: "Descargar",
        cache: "Caché del juego",
        documentation: "Documentación"
    },

    pl: {
        title: "Mass Effect 1 na Androida",
        description: "Odkryj legendarny świat Mass Effect 1, teraz dostępny na Androidzie. Eksploruj odległe planety, podejmuj ważne decyzje, zbieraj drużynę i wyrusz wraz z komandorem Shepardem w niezapomnianą podróż, aby ocalić galaktykę. Wielka przygoda zaczyna się w Twojej dłoni.",

        download: "Pobierz",
        cache: "Pamięć podręczna gry",
        documentation: "Dokumentacja"
    },

    cs: {
        title: "Mass Effect 1 pro Android",
        description: "Objev legendární vesmír Mass Effect 1, nyní na Androidu. Prozkoumávej vzdálené světy, rozhoduj o osudu galaxie, sestav svůj tým a vydej se s velitelem Shepardem na nezapomenutelnou cestu za záchranou galaxie. Dobrodružství začíná přímo ve tvých rukou.",

        download: "Stáhnout",
        cache: "Mezipaměť hry",
        documentation: "Dokumentace"
    },

    hu: {
        title: "Mass Effect 1 Androidra",
        description: "Fedezd fel a Mass Effect 1 legendás univerzumát, most Androidon. Fedezz fel távoli világokat, hozz sorsfordító döntéseket, állítsd össze csapatodat, és kísérd el Shepard parancsnokot egy felejthetetlen küldetésre, hogy megmentsétek a galaxist. A kaland most a tenyeredben kezdődik.",

        download: "Letöltés",
        cache: "Játék gyorsítótára",
        documentation: "Dokumentáció"
    }
};


function setLanguage(lang) {
    if (!translations[lang]) return;

    document.querySelectorAll("[data-i18n]").forEach(element => {
        const key = element.dataset.i18n;

        if (translations[lang][key]) {
            element.textContent = translations[lang][key];
        }
    });

    document.documentElement.lang = lang;
    localStorage.setItem("language", lang);
}

const savedLanguage = localStorage.getItem("language") || "en";
setLanguage(savedLanguage);


