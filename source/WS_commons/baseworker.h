#pragma once
#include <string>
#include <map>
#include <boost/function.hpp>
#include "threadpool.h"
#include <boost/bind.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string.hpp>
#include <vector>
#include <chrono>
#include <boost/accumulators/accumulators.hpp>
#include <boost/accumulators/statistics/mean.hpp>
#include <boost/accumulators/statistics/max.hpp>
#include <boost/accumulators/statistics/count.hpp>


#include <boost/lexical_cast.hpp>


namespace webservice
{

    
    void decode(std::string& str){
        size_t pos = -1;
        int code = 0;
        std::stringstream ss;
        std::string number;
        while((pos = str.find("%", pos+1)) != std::string::npos){
            number = str.substr(pos+1, 2);
            if(number.size() < 2){
                continue;
            }
            ss << std::hex << number;
            ss >> code;
            ss.clear();
            if(code < 32){
                continue;
            }
            str.replace(pos, 3, 1, static_cast<char>(code));
        }

    }

    using namespace boost::accumulators;
    /** Contient des données statiques aux workers */
    struct StaticData {
        typedef accumulator_set<double, features<tag::max, tag::mean, tag::count> > mean_t;
        std::map<std::string, mean_t> means;
    };

    StaticData & static_data() {
        static StaticData ans;
        return ans;
    }

    /** Worker de base. Doit être hérité pour définir le comportement et les données souhaitées pour chaque thread */
    template<class Data>
    class BaseWorker {
    public: // Structures de données

        /** Type d'une fonction qui prend RequestData et Data en paramètre et qui retourne ResponseData */
        typedef boost::function<ResponseData(RequestData, Data & )> ApiFun;

        /** Ensemble des fonctions des apis indexées par leur nom */
        std::map<std::string, ApiFun> apis;

        /** Metadonnées des api indexées par leur nom */
        std::map<std::string, ApiMetadata> api_metadata;
    
    public:

        /**
         * Méthode appelée juste avant l'appel de l'api séléctionnée
         * il faut surcharger cette methode dans le worker pour ajouter des traitements
         */
        virtual void pre_compute(webservice::RequestData&, Data& ){}
        /**
         * Méthode appelée juste aprés l'appel de l'api séléctionnée
         * il faut surcharger cette methode dans le worker pour ajouter des traitements
         */
        virtual void post_compute(webservice::RequestData&, webservice::ResponseData&){}

        /** Méthode appelée si l'appele lève une exception standart
         ** il faut surcharger cette methode dans le worker pour ajouter des traitements
         **/
        virtual void on_std_exception(const std::exception &, webservice::RequestData&, webservice::ResponseData&, Data &) {throw;}

        /** Méthode appelée si l'appele lève une exception inconnue
         ** il faut surcharger cette methode dans le worker pour ajouter des traitements
         **/
        virtual void on_unknown_exception(webservice::RequestData&, webservice::ResponseData&, Data &) {throw;}

        /** Fonction appelée lorsqu'une requête appelle
      *
      * Cette fonction dispatche ensuite à la bonne en fonction de l'appel
      * Elle découpe également la chaine de requêtes (en POST et GET)
      * et mets le tout dans le dictionnaire request.params
      */
        webservice::ResponseData dispatch(webservice::RequestData request, Data & d){
            decode(request.path);
            std::string raw_params = request.raw_params;
            decode(raw_params);
            decode(request.data);
            std::vector<std::string> tokens, tokens2;

            boost::algorithm::split(tokens, request.data, boost::algorithm::is_any_of("&"));
            boost::algorithm::split(tokens2, raw_params, boost::algorithm::is_any_of("&"));
            for(auto token : tokens2) tokens.push_back(token);

            for(std::string token : tokens) {
                size_t pos = token.find("=");
                if(pos != std::string::npos && token != "")
                    request.params.insert(std::make_pair(boost::algorithm::to_lower_copy(token.substr(0, pos)), token.substr(pos +1)));
                else if(token.size() > 0 && token[0] != '=')
                    request.params.insert(std::make_pair(boost::algorithm::to_lower_copy(token), ""));
            }            

            size_t position = request.path.find_last_of('/');
            std::string api;
            if(position != std::string::npos){
                api = request.path.substr(position+1);
            }else{
                api = request.path;
            }
            boost::algorithm::to_lower(api);

            if(apis.find(api) == apis.end()) {
                ResponseData resp;
                resp.content_type = "text/xml";
                resp.response << "<error>API inconnue</error>";
                return resp;
            }else{
                
                api_metadata[api].parse_parameters(request);
                api_metadata[api].check_manadatory_parameters(request);

                boost::posix_time::ptime start(boost::posix_time::microsec_clock::local_time());
                pre_compute(request, d);
                ResponseData resp;
              //  try{
                    resp = apis[api](request, d);
               /* } catch(const std::exception & e){
                    on_std_exception(e, request, resp, d);
                } catch(...){
                    on_unknown_exception(request, resp, d);
                }*/

                resp.api = api;
                post_compute(request, resp);
                //@TODO not threadsafe
                int duration = (boost::posix_time::microsec_clock::local_time() - start).total_milliseconds();
                static_data().means[api](duration);
                return resp;
            }
        }


     /** Permet de rajouter une nouvelle api
      *
      * Le nom est celui utilisé dans la requête
      * La fonction (idéalement vers un membre du Worker pour profiter des données) va effectuer le traitement
      * La description est pour fournir l'aide à l'utilisateur
      *
      * Si la fonction est une methode membre, utiliser boost::bind(&Classe::methode, this, _1, _2)
      */
        void register_api(std::string name, ApiFun fun, const std::string & description){
            boost::algorithm::to_lower(name);
            apis[name] = fun;

            ApiMetadata api;
            api.description = description;
            api_metadata[name] = api;

        }

        /** Rajoute un paramètre à une api.
      *
      * Retourne false si l'api n'existe pas.
      * Si le paramètre avait déjà été défini, il est écrasé
      */
        bool add_param(std::string api, std::string name,
                       const std::string & description, enum ApiParameter::Type_p type, bool mandatory, const std::vector<RequestParameter::Parameter_variant>& accepted_values = std::vector<RequestParameter::Parameter_variant>()) {
            boost::algorithm::to_lower(api);
            boost::algorithm::to_lower(name);
            if(api_metadata.find(api) == api_metadata.end()) {
                return false;
            }

            ApiParameter param;
            param.description = description;
            param.type = type;
            param.mandatory = mandatory;
            param.accepted_values = accepted_values;
            api_metadata[api].params[name] = param;
            return true;
        }


        /** Fonction appelée à chaque requête */
        ResponseData operator()(const RequestData & request, Data & d){
            return dispatch(request,d);
        }

        // Quelques APIs par défaut
        /** Liste l'ensemble des APIs disponibles avec les paramètres */
        ResponseData help(RequestData, Data &){
            ResponseData rd;
            rd.content_type = "text/html";
            rd.status_code = 200;
            rd.response << "<html><head><title>Liste des API</title></head><body>\n"
                    << "<h1>Liste des APIs</h1>\n";
            for(auto api : api_metadata){
                rd.response << "<h2>" << api.first << "</h2>\n"
                        << "<h3>Description</h3><p>" << api.second.description << "</p>\n"
                        << "<h3>Paramètres</h3><table border=1>"
                        << "<tr><th>Paramètre</th><th>Type</th><th>Description</th><th>Obligatoire</th></tr>\n";
                for(auto param : api.second.params){
                    std::string type_caption;
                    switch(param.second.type){
                    case ApiParameter::STRING: type_caption = "Chaîne de caractères"; break;
                    case ApiParameter::INT: type_caption = "Entier"; break;
                    case ApiParameter::DOUBLE: type_caption = "Réel p.ex. 3.14"; break;
                    case ApiParameter::DATE: type_caption = "Date p.ex. 20111031"; break;
                    case ApiParameter::TIME: type_caption = "Heure p.ex 0918"; break;
                    case ApiParameter::DATETIME: type_caption = "Date-heure p.ex. 20111031T0918"; break;
                    case ApiParameter::BOOLEAN: type_caption = "Booléen 1 pour vrai, 0 pour faux"; break;
                    case ApiParameter::STRINGLIST: type_caption = "Liste de chaînes de caractères p.ex. bonjour;le;monde"; break;
                    }

                    rd.response << "<tr><td>" << param.first << "</td><td>" << type_caption << "</td><td>" << param.second.description << "</td><td>" << param.second.mandatory << "</td></tr>\n";
                }
                rd.response << "</table>\n";
            }
            rd.response << "</body></html>";
            return rd;
        }

        /** Affiche de statistiques sur l'utilisation de chaque API */
        ResponseData stats(RequestData, Data &) {
            ResponseData rd;
            rd.content_type = "text/html";
            rd.status_code = 200;
            rd.response << "<html><head><title>Statistiques</title></head><body>\n"
                    << "<h1>Statistiques</h1>";
            for(auto api : apis) {
                rd.response << "<h2>" << api.first << "</h2>\n"
                        << "<p>Temps moyen (ms) : " << mean(static_data().means[api.first]) << "<br/>\n"
                        << "Temps max d'appel (ms) : " << (max)(static_data().means[api.first]) << "<br/>\n"
                        << "Nombre d'appels : " << count(static_data().means[api.first]) << "<br/></p>\n";
            }
            rd.response << "</body></html>";
            return rd;
        }

        /** Analyse les paramêtres d'une requête */
        ResponseData analyze(RequestData request, Data &){
            ResponseData rd;
            rd.content_type = "text/html";
            rd.status_code = 200;
            rd.response << "<html><head><meta http-equiv=\"Content-Type\" content=\"text/html; charset=utf-8\"><title>analyse</title></head><body>\n"
                    << "<h1>Analyse de requête</h1>";

            if(request.params.find("api") == request.params.end()){
                rd.response << "<p>Il faut préciser l'API que vous souhaitez interroger avec api=nom_api</p>"
                            << "</body></html>";
                return rd;
            }

            std::string api = request.params.find("api")->second;

            if(apis.find(api) == apis.end()){
                rd.response << "<p>L'API " << api << " est inconnue"
                            << "</body></html>";
                return rd;
            }

            rd.response << "<h2> Paramètres obligatoires </h2>"
                        << "<ul>";

            for(std::pair<std::string, ApiParameter> ap : api_metadata[api].params){
                if(ap.second.mandatory){
                    rd.response << "<li><b>" << ap.first << "</b> : ";
                        if(request.params.find(ap.first) == request.params.end())
                            rd.response << " Absent ! </li>";
                        else
                            rd.response << " Présent</li>";
                }
            }

            rd.response << "</ul>"
                        << "<h2>Valeurs</h2>"
                        << "<table border=\"1\">"
                        << "<tr><td>nom</td><td>validité</td><td>valeur parsé</td><td>valeur originel</td></tr>";

            for(std::pair<std::string, std::string> sp : request.params){
                if(sp.first != "api"){
                    RequestParameter rp = api_metadata[api].convert_parameter(sp.first, sp.second);
                    rd.response << "<tr><td>" << sp.first << "</td>";
                    if(rp.used_value){
                        if(rp.valid_value)
                            rd.response << "<td>format valide</td>";
                        else
                            rd.response << "<td>format non valide</td>";
                    }else {
                        rd.response << "<td>paramètre ignoré par l'API</td>";
                    }
                    rd.response << "<td>" << sp.second << "</td><td>";
                    boost::apply_visitor(PrintParameterVisitor(rd.response), request.parsed_params[sp.first].value);
                    rd.response << "</td></tr>";
                }
            }
            rd.response << "</table>"
                        << "</body></html>";

            return rd;
        }

        /** Ajoute quelques API par défaut
          *
          * help : donne une aide listant toutes les api disponibles et leurs paramètres
          * status : donne des informations sur le webservice
          */
        void add_default_api() {
            register_api("help", boost::bind(&BaseWorker<Data>::help, this, _1, _2), "Liste des APIs utilisables");
            register_api("stats", boost::bind(&BaseWorker<Data>::stats, this, _1, _2), "Statistiques sur les appels d'api");
            register_api("analyze", boost::bind(&BaseWorker<Data>::analyze, this, _1, _2), "Analyze une requête");
            add_param("analyze", "api", "Nom de l'api à analyser. Rajouter ensuite tous les paramètres habituels", ApiParameter::STRING, true);
        }


        std::string run_query(const std::string & request, Data & d){
            std::stringstream result;
            auto start = std::chrono::high_resolution_clock::now();
            webservice::RequestData query;
            query.method = webservice::GET;

            size_t pos = request.find("?");
            if(pos == std::string::npos){
                query.path = request;
            }else{
                query.path = request.substr(0, pos);
                query.raw_params = request.substr(pos+1);
            }
            auto response = dispatch(query, d);

            auto end = std::chrono::high_resolution_clock::now();
            result << "Durée d'exécution : " << std::chrono::duration_cast<std::chrono::milliseconds>(end-start).count() << "ms" << std::endl;
            result << response.response.str() << std::endl;
            return result.str();
        }
    };

}
