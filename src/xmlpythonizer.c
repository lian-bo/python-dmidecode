/*   Converts XML docs and nodes to Python dicts and lists by
 *   using an XML file which describes the Python dict layout
 *
 *   Copyright 2009      David Sommerseth <davids@redhat.com>
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 *
 *   For the avoidance of doubt the "preferred form" of this code is one which
 *   is in an open unpatent encumbered format. Where cryptographic key signing
 *   forms part of the process of creating an executable the information
 *   including keys needed to generate an equivalently functional executable
 *   are deemed to be part of the source code.
 */

#include <Python.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <libxml/tree.h>
#include <libxml/xpath.h>

#include "dmixml.h"
#include "xmlpythonizer.h"

ptzMAP *ptzmap_Add(const ptzMAP *chain,
                   ptzTYPES ktyp, const char *key,
                   ptzTYPES vtyp, const char *value,
                   const char *filter, const char *filterval,
                   ptzMAP *child)
{
        ptzMAP *ret = NULL;

        assert( (ktyp == ptzCONST) || (ktyp == ptzSTR) || (ktyp == ptzINT) || (ktyp == ptzFLOAT) );
        assert( key != NULL );
        // Make sure that value and child are not used together
        assert( ((value == NULL) && child != NULL) || ((value != NULL) && (child == NULL)) );

        ret = (ptzMAP *) malloc(sizeof(ptzMAP)+2);
        assert( ret != NULL );
        memset(ret, 0, sizeof(ptzMAP)+2);

        ret->type_key = ktyp;
        ret->key = strdup(key);

        ret->type_value = vtyp;
        if( value != NULL ) {
                ret->value = strdup(value);
                ret->child = NULL;
        } else if( child != NULL ) {
                ret->value = NULL;
                ret->child = child;
        }

        if( filter != NULL && filterval != NULL ) {
                ret->filter = strdup(filter);
                ret->filtervalue = strdup(filterval);
        }

        if( chain != NULL ) {
                ret->next = (ptzMAP *) chain;
        }
        return ret;
};

void ptzmap_Free_func(ptzMAP *ptr)
{
        if( ptr == NULL ) {
                return;
        }

        free(ptr->key);
        ptr->key = NULL;

        if( ptr->value != NULL ) {
                free(ptr->value);
                ptr->value = NULL;
        }

        if( ptr->filter != NULL ) {
                free(ptr->filter);
                ptr->filter = NULL;
        }

        if( ptr->filtervalue != NULL ) {
                free(ptr->filtervalue);
                ptr->filtervalue = NULL;
        }

        if( ptr->child != NULL ) {
                ptzmap_Free(ptr->child);
        }
        if( ptr->next != NULL ) {
                ptzmap_Free(ptr->next);
        }
        free(ptr);
}


#if 0
// DEBUG FUNCTIONS
static const char *ptzTYPESstr[] = { "ptzCONST", "ptzSTR", "ptzINT", "ptzFLOAT", "ptzBOOL",
                                     "ptzLIST_STR", "ptzLIST_INT", "ptzLIST_FLOAT", "ptzLIST_BOOL",
                                     "ptzDICT", NULL };

void indent(int lvl)
{
        int i = 0;
        if( lvl == 0 ) {
                return;
        }

        for( i = 0; i < (lvl * 3); i++ ) {
                printf(" ");
        }
}

#define ptzmap_Dump(ptr) { ptzmap_Dump_func(ptr, 0); }
void ptzmap_Dump_func(const ptzMAP *ptr, int level)
{
        if( ptr == NULL ) {
                return;
        }

        indent(level); printf("key type:   (%i) %-13.13s - key:   %s\n",
                              ptr->type_key, ptzTYPESstr[ptr->type_key], ptr->key);
        indent(level); printf("value type: (%i) %-13.13s - value: %s\n",
                              ptr->type_value, ptzTYPESstr[ptr->type_value], ptr->value);
        if( ptr->filter != NULL ) {
                indent(level); printf("filter: %s == %s\n", ptr->filter, ptr->filtervalue);
        }

        if( ptr->child != NULL ) {
                indent(level); printf(" ** CHILD\n");
                ptzmap_Dump_func(ptr->child, level + 1);
                indent(level); printf(" ** ---------\n");
        }
        if( ptr->next != NULL ) {
                printf("\n");
                ptzmap_Dump_func(ptr->next, level);
        }
}
#endif // END OF DEBUG FUNCTIONS

//
//  Parser for the XML -> Python mapping XML file
//
//  This mappipng XML file describes how the Python result
//  should look like and where it should pick the data from
//  when later on parsing the dmidecode XML data.
//

// Valid key and value types for the mapping file
inline ptzTYPES _convert_maptype(const char *str) {
        if( strcmp(str, "string") == 0 ) {
                return ptzSTR;
        } else if( strcmp(str, "constant") == 0 ) {
                return ptzCONST;
        } else if( strcmp(str, "integer") == 0 ) {
                return ptzINT;
        } else if( strcmp(str, "float") == 0 ) {
                return ptzFLOAT;
        } else if( strcmp(str, "boolean") == 0 ) {
                return ptzBOOL;
        } else if( strcmp(str, "list:string") == 0 ) {
                return ptzLIST_STR;
        } else if( strcmp(str, "list:integer") == 0 ) {
                return ptzLIST_INT;
        } else if( strcmp(str, "list:float") == 0 ) {
                return ptzLIST_FLOAT;
        } else if( strcmp(str, "list:boolean") == 0 ) {
                return ptzLIST_BOOL;
        } else if( strcmp(str, "dict") == 0 ) {
                return ptzDICT;
        } else {
                fprintf(stderr, "Unknown field type: %s - defaulting to 'string'\n", str);
                return ptzSTR;
        }
}

// Internal parser
ptzMAP *_do_dmimap_parsing(xmlNode *node) {
        ptzMAP *retmap = NULL;
        xmlNode *ptr_n = NULL, *map_n = NULL;;

        // Go to the next XML_ELEMENT_NODE
        for( map_n = node; map_n != NULL; map_n = map_n->next ) {
                if( map_n->type == XML_ELEMENT_NODE ) {
                        break;
                }
        }
        if( map_n == NULL ) {
                return NULL;
        }

        // Go to the first <Map> node
        if( xmlStrcmp(node->name, (xmlChar *) "Map") != 0 ) {
                map_n = dmixml_FindNode(node, "Map");
                if( map_n == NULL ) {
                        return NULL;
                }
        }

        // Loop through it's children
        for( ptr_n = map_n ; ptr_n != NULL; ptr_n = ptr_n->next ) {
                ptzTYPES type_key, type_value;
                char *key = NULL, *value = NULL;
                char *filter = NULL, *filterval = NULL;

                if( ptr_n->type != XML_ELEMENT_NODE ) {
                        continue;
                }

                // Get the attributes defining key, keytype, value and valuetype
                key = dmixml_GetAttrValue(ptr_n, "key");
                type_key = _convert_maptype(dmixml_GetAttrValue(ptr_n, "keytype"));

                value = dmixml_GetAttrValue(ptr_n, "value");
                type_value = _convert_maptype(dmixml_GetAttrValue(ptr_n, "valuetype"));

                // Get filters
                filter = dmixml_GetAttrValue(ptr_n, "filter");
                if( filter != NULL ) {
                        filterval = dmixml_GetAttrValue(ptr_n, "filtervalue");
                }

                if( type_value == ptzDICT ) {
                        // When value type is ptzDICT, traverse the children nodes
                        // - should contain another Map set instead of a value attribute
                        if( ptr_n->children == NULL ) {
                                continue;
                        }
                        // Recursion
                        retmap = ptzmap_Add(retmap, type_key, key,
                                            type_value, NULL,
                                            filter, filterval,
                                            _do_dmimap_parsing(ptr_n->children->next));

                } else {
                        // Append the value as a normal value when the
                        // value type is not a Python Dict
                        retmap = ptzmap_Add(retmap, type_key, key,
                                            type_value, value,
                                            filter, filterval,
                                            NULL);
                }
                value = NULL;
                key = NULL;
        }
        return retmap;
}

// Main parser function for the mapping XML
ptzMAP *dmiMAP_ParseMappingXML(xmlDoc *xmlmap, const char *mapname) {
        ptzMAP *map = NULL;
        xmlNode *node = NULL;

        // Find the root tag and locate our mapping
        node = xmlDocGetRootElement(xmlmap);
        assert( node != NULL );

        // Verify that the root node got the right name
        if( (node == NULL)
            || (xmlStrcmp(node->name, (xmlChar *) "dmidecode_fieldmap") != 0 )) {
                fprintf(stderr, "Invalid XML-Python mapping file\n");
                return NULL;
        }

        // Verify that it's of a version we support
        if( strcmp(dmixml_GetAttrValue(node, "version"), "1") != 0 ) {
                fprintf(stderr, "Unsupported XML-Python mapping file format\n");
                return NULL;
        }

        // Find the <Mapping> section matching our request (mapname)
        for( node = node->children->next; node != NULL; node = node->next ) {
                if( xmlStrcmp(node->name, (xmlChar *) "Mapping") == 0) {
                        char *name = dmixml_GetAttrValue(node, "name");
                        if( (name != NULL) && (strcmp(name, mapname) == 0) ) {
                                break;
                        }
                }
        }

        if( node == NULL ) {
                fprintf(stderr, "No mapping for '%s' was found "
                        "in the XML-Python mapping file\n", mapname);
                return NULL;
        }

        // Start creating an internal map structure based on the mapping XML.
        map = _do_dmimap_parsing(node);
        return map;
}


//
//  Parser routines for converting XML data into Python structures
//

inline PyObject *StringToPyObj(ptzTYPES type, const char *str) {
        PyObject *value;

        if( str == NULL ) {
                return Py_None;
        }

        switch( type ) {
        case ptzINT:
        case ptzLIST_INT:
                value = PyInt_FromLong(atoi(str));
                break;

        case ptzFLOAT:
        case ptzLIST_FLOAT:
                value = PyFloat_FromDouble(atof(str));
                break;

        case ptzBOOL:
        case ptzLIST_BOOL:
                value = PyBool_FromLong((atoi(str) == 1 ? 1:0));
                break;

        case ptzSTR:
        case ptzLIST_STR:
                value = PyString_FromString(str);
                break;

        default:
                fprintf(stderr, "Invalid type '%i' for value '%s'\n", type, str);
                value = Py_None;
        }
        return value;
}


// Retrieve a value from the XML doc (XPath Context) based on a XPath query
xmlXPathObject *_get_xpath_values(xmlXPathContext *xpctx, const char *xpath) {
        xmlChar *xp_xpr = NULL;
        xmlXPathObject *xp_obj = NULL;

        if( xpath == NULL ) {
                return NULL;
        }

        xp_xpr = xmlCharStrdup(xpath);
        xp_obj = xmlXPathEvalExpression(xp_xpr, xpctx);
        assert( xp_obj != NULL );
        free(xp_xpr);

        if( (xp_obj->nodesetval == NULL) || (xp_obj->nodesetval->nodeNr == 0) ) {
                xmlXPathFreeObject(xp_obj);
                return NULL;
        }

        return xp_obj;
}

inline char *_get_key_value(ptzMAP *map_p, xmlXPathContext *xpctx, int idx) {
        char *key = NULL;
        xmlXPathObject *xpobj = NULL;

        switch( map_p->type_key ) {
        case ptzCONST:
                key = map_p->key;
                break;

        case ptzSTR:
        case ptzINT:
        case ptzFLOAT:
                xpobj = _get_xpath_values(xpctx, map_p->key);
                if( xpobj != NULL ) {
                        key = dmixml_GetContent(xpobj->nodesetval->nodeTab[idx]);
                        xmlXPathFreeObject(xpobj);
                }
                break;

        default:
                fprintf(stderr, "Unknown key type: %i\n", map_p->type_key);
                break;
        }
        if( key == NULL ) {
                fprintf(stderr, "Could not find the key value: %s\n", map_p->key);
        }
        return key;
}


void _register_value(PyObject *pyobj, xmlXPathContext *xpctx,
                     ptzMAP *map_p, const char *key, PyObject *value)
{
        if( key != NULL ) {
                if( (map_p->filter != NULL) && (map_p->filtervalue != NULL) ) {
                        xmlXPathObject *xpobj = NULL;
                        int i = 0, found = 0;

                        xpobj = _get_xpath_values(xpctx, map_p->filter);
                        if( (xpobj == NULL) || (xpobj->nodesetval->nodeNr < 1) ) {
                                // If node not found, or our value index is higher
                                // than what we found, filter it out.
                                if( xpobj != NULL ) {
                                        xmlXPathFreeObject(xpobj);
                                }
                                return;
                        }

                        for( i = 0; i < xpobj->nodesetval->nodeNr; i++ ) {
                                if( strcmp(map_p->filtervalue,
                                   dmixml_GetContent(xpobj->nodesetval->nodeTab[i])) == 0 ) {
                                        found = 1;
                                        break;
                                }
                        }

                        if( found != 1 ) {
                                // If value did not match - no hit => do not add value
                                xmlXPathFreeObject(xpobj);
                                return;
                        }
                }
                PyDict_SetItemString(pyobj, key, value);
                Py_DECREF(value);
        }
}

// Internal XML parser routine, which traverses the given mapping table,
// returning a Python structure accordingly to the map.
PyObject *_do_pythonizeXML(ptzMAP *in_map, xmlXPathContext *xpctx, int lvl) {
        ptzMAP *map_p = NULL;
        PyObject *retdata = NULL;
        int i = 0;

        retdata = PyDict_New();
        for( map_p = in_map; map_p != NULL; map_p = map_p->next ) {
                xmlXPathObject *xpobj = NULL;
                char *key = NULL;
                PyObject *value = NULL;

                // Get 'value' value
                switch( map_p->type_value ) {
                case ptzCONST:
                        value = PyString_FromString(map_p->value);
                        key = _get_key_value(map_p, xpctx, 0);
                        _register_value(retdata, xpctx, map_p, key, value);

                        break;

                case ptzSTR:
                case ptzINT:
                case ptzFLOAT:
                case ptzBOOL:
                        xpobj = _get_xpath_values(xpctx, map_p->value);
                        if( xpobj != NULL ) {
                                for( i = 0; i < xpobj->nodesetval->nodeNr; i++ ) {
                                        value = StringToPyObj(map_p->type_value,
                                                              dmixml_GetContent(xpobj->nodesetval->nodeTab[i]));
                                        key = _get_key_value(map_p, xpctx, i);
                                        _register_value(retdata, xpctx, map_p, key, value);
                                }
                                xmlXPathFreeObject(xpobj);
                        }
                        break;

                case ptzLIST_STR:
                case ptzLIST_INT:
                case ptzLIST_FLOAT:
                case ptzLIST_BOOL:
                        xpobj = _get_xpath_values(xpctx, map_p->value);
                        value = PyList_New(0);
                        if( xpobj != NULL ) {
                                for( i = 0; i < xpobj->nodesetval->nodeNr; i++ ) {
                                        char *valstr = dmixml_GetContent(xpobj->nodesetval->nodeTab[i]);
                                        PyList_Append(value, StringToPyObj(map_p->type_value, valstr));
                                }
                                xmlXPathFreeObject(xpobj);
                                key = _get_key_value(map_p, xpctx, 0);
                                _register_value(retdata, xpctx, map_p, key, value);
                        }
                        break;

                case ptzDICT:
                        // Traverse the children to get the value of this element
                        value = _do_pythonizeXML(map_p->child, xpctx, lvl+1);
                        Py_DECREF(value);

                        key = _get_key_value(map_p, xpctx, 0);
                        _register_value(retdata, xpctx, map_p, key, value);
                        break;

                default:
                        fprintf(stderr, "Unknown value type: %i\n", map_p->type_value);
                        return Py_None;
                        break;
                }
        }
        Py_INCREF(retdata);
        return retdata;
}

// Convert a xmlDoc to a Python object, based on the given map
PyObject *pythonizeXMLdoc(ptzMAP *map, xmlDoc *xmldoc)
{
        xmlXPathContext *xp_ctx = NULL;
        PyObject *retdata = NULL;

        // Prepare a XPath context for XPath queries
        xp_ctx = xmlXPathNewContext(xmldoc);
        assert( xp_ctx != NULL );

        // Parse the XML and create Python data
        retdata = _do_pythonizeXML(map, xp_ctx, 0);

        // Clean up and return data
        xmlXPathFreeContext(xp_ctx);

        return retdata;
}

// Convert a xmlNode to a Python object, based on the given map
PyObject *pythonizeXMLnode(ptzMAP *map, xmlNode *nodes)
{
        xmlDoc *xmldoc = NULL;
        PyObject *retdata = NULL;

        // Create our own internal XML doc and
        // copy over the input nodes to our internal doc.
        // This is needed as the XPath parser in libxml2
        // only works with xmlDoc.
        xmldoc = xmlNewDoc((xmlChar *) "1.0");
        assert( xmldoc != NULL );
        xmlDocSetRootElement(xmldoc, xmlCopyNode(nodes, 1));

        // Parse the internal xmlDoc
        retdata = pythonizeXMLdoc(map, xmldoc);

        // Clean up and return data
        xmlFreeDoc(xmldoc);
        return retdata;
}



#if 0
// Simple independent main function - only for debugging
int main() {
        xmlDoc *doc = NULL, *data = NULL;
        ptzMAP *map = NULL;
        PyObject *pydat = NULL;

        doc = xmlReadFile("pythonmap.xml", NULL, 0);
        assert( doc != NULL );

        map = dmiMAP_ParseMappingXML(doc, "bios");
        ptzmap_Dump(map);
        printf("----------------------\n");


        data = xmlReadFile("test.xml", NULL, 0);
        assert( data != NULL );

        pydat = pythonizeXMLdoc(map, data);
        Py_INCREF(pydat);
        PyObject_Print(pydat, stdout, 0);
        Py_DECREF(pydat);

        ptzmap_Free(map);
        xmlFreeDoc(data);
        xmlFreeDoc(doc);

        return 0;
}
#endif

#if 0
// Simple test module for Python - only for debugging
PyObject* demo_xmlpy()
{
        xmlDoc *doc = NULL, *mapping_xml = NULL;
        ptzMAP *mapping = NULL;
        PyObject *ret = NULL;

        // Read the XML-Python mapping setup
        mapping_xml = xmlReadFile("pythonmap.xml", NULL, 0);
        assert( mapping_xml != NULL );

        mapping = dmiMAP_ParseMappingXML(mapping_xml, "bios");
        assert( mapping != NULL );

        // Read XML data from file
        doc = xmlReadFile("test.xml", NULL, 0);
        assert( doc != NULL );

        // Create a PyObject out of the XML indata
        ret = pythonizeXMLdoc(mapping, doc);

        // Clean up and return the data
        ptzmap_Free(mapping);
        xmlFreeDoc(doc);
        xmlFreeDoc(mapping_xml);

        return ret;
}


static PyMethodDef DemoMethods[] = {
        {"xmlpy", demo_xmlpy, METH_NOARGS, ""},
        {NULL, NULL, 0, NULL}
};

PyMODINIT_FUNC initxmlpythonizer(void) {
        PyObject *module =
                Py_InitModule3((char *)"xmlpythonizer", DemoMethods,
                               "XML to Python Proof-of-Concept Python Module");

        PyObject *version = PyString_FromString("2.10");
        Py_INCREF(version);
        PyModule_AddObject(module, "version", version);
}
#endif // Python test module

